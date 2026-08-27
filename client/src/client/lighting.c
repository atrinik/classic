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

/**
 * @file
 * Software-rendered, per-pixel map lighting.
 */

#include <global.h>
#include <lighting.h>

#include "lighting_lut.inc"

#define LIGHTING_MAX_DIRTY_RECTS 64
#define LIGHTING_MAX_DIRTY_SPANS 8
#define LIGHTING_SCROLL_MARGIN 64
#define LIGHTING_MAP_LEVEL_PIXEL_HEIGHT 46

typedef struct lighting_sample lighting_sample;

struct lighting_sample {
    uint16_t scalar;
    uint16_t red;
    uint16_t green;
    uint16_t blue;
    uint8_t present;
    uint8_t reserved;
};

typedef struct lighting_sprite_cache_key {
    SDL_Surface *source;
    int32_t source_x;
    int32_t source_y;
    int32_t source_w;
    int32_t source_h;
    uint64_t illumination_signature;
    uint8_t mode;
    uint8_t surface_alpha;
} lighting_sprite_cache_key;

typedef struct lighting_sprite_cache_entry {
    lighting_sprite_cache_key key;
    lighting_sample *illumination;
    size_t illumination_count;
    int *structure_column_bottom;
    SDL_Surface *surface;
    size_t bytes;
    struct lighting_sprite_cache_entry *lru_previous;
    struct lighting_sprite_cache_entry *lru_next;
    UT_hash_handle hh;
} lighting_sprite_cache_entry;

typedef struct lighting_dirty_rect {
    int x0;
    int y0;
    int x1;
    int y1;
} lighting_dirty_rect_t;

typedef struct lighting_dirty_span {
    int x0;
    int x1;
} lighting_dirty_span_t;

typedef struct lighting_interval {
    int start;
    int end;
} lighting_interval_t;

typedef enum lighting_full_rebuild_cause {
    LIGHTING_FULL_REBUILD_NONE,
    LIGHTING_FULL_REBUILD_CACHE,
    LIGHTING_FULL_REBUILD_ACTIVE,
    LIGHTING_FULL_REBUILD_BOUNDS,
    LIGHTING_FULL_REBUILD_CONTROL,
} lighting_full_rebuild_cause_t;

typedef enum lighting_sprite_invalidation_cause {
    LIGHTING_SPRITE_INVALIDATION_FIELD,
    LIGHTING_SPRITE_INVALIDATION_SCROLL,
    LIGHTING_SPRITE_INVALIDATION_SOURCE,
    LIGHTING_SPRITE_INVALIDATION_RESET,
    LIGHTING_SPRITE_INVALIDATION_EVICTION,
} lighting_sprite_invalidation_cause_t;

_Static_assert(sizeof(lighting_sample) == 10U,
               "lighting samples require a padding-free ten-byte representation");

typedef struct lighting_context {
    lighting_sample *samples;
    lighting_sample *structure_samples;
    lighting_sample *structure_illumination;
    lighting_sample *structure_blur_row;
    uint8_t *rows_valid;
    uint8_t *structure_illumination_valid;
    size_t samples_num;
    int width;
    int height;
    /* Logical screen coordinates are kept in a circular field.  Scrolling
     * changes these origins instead of copying the complete viewport. */
    int sample_origin_x;
    int sample_origin_y;
    bool active;
    bool cache_valid;
    bool update_needed;
    uint64_t cache_key;
    uint64_t pending_cache_key;
    lighting_dirty_rect_t dirty[LIGHTING_MAX_DIRTY_RECTS];
    size_t dirty_num;
    lighting_dirty_span_t *dirty_spans;
    uint8_t *dirty_span_num;
    bool dirty_span_overflow;
    lighting_full_rebuild_cause_t full_rebuild_cause;
    lighting_sprite_cache_entry *sprite_cache;
    lighting_sprite_cache_entry *sprite_cache_lru_oldest;
    lighting_sprite_cache_entry *sprite_cache_lru_newest;
    size_t sprite_cache_bytes;
    lighting_benchmark_counters_t benchmark_counters;
    lighting_benchmark_timings_t benchmark_timings;
    size_t benchmark_peak_sprite_entries;
    size_t benchmark_peak_sprite_bytes;
    size_t benchmark_peak_retained_field_bytes;
} lighting_context;

static lighting_context lighting_contexts[MAP2_LEVELS];
static lighting_context *lighting_context_current = &lighting_contexts[MAP2_DEPTH_INDEX(0)];
static lighting_benchmark_statistics_t lighting_benchmark_statistics;
static bool lighting_benchmark_timing_enabled;
static lighting_benchmark_reconstruction_t lighting_benchmark_reconstruction =
    LIGHTING_BENCHMARK_RECONSTRUCTION_TRANSLATED;
#ifdef ATRINIK_WIDGET_TESTS
enum {
    LIGHTING_BENCHMARK_FAULT_CREATE = UINT8_C(1) << 0,
    LIGHTING_BENCHMARK_FAULT_STRUCTURE_LOCK = UINT8_C(1) << 1,
    LIGHTING_BENCHMARK_FAULT_PROJECTED_LOCK = UINT8_C(1) << 2,
    LIGHTING_BENCHMARK_FAULT_DESTINATION_LOCK = UINT8_C(1) << 3,
    LIGHTING_BENCHMARK_FAULT_SOURCE_LIFETIME = UINT8_C(1) << 4,
};
static bool lighting_benchmark_fault_enabled;
static uint8_t lighting_benchmark_fault_observed;
static uint8_t lighting_benchmark_fault_expected;
#endif
static SDL_Surface *lighting_lit_surface;
static int *structure_column_bottom;
static lighting_sample *structure_column_illumination;
/* Painter ownership for the current complete scene. A signed byte is enough
 * for the bounded MAP2 depth range and keeps the ownership buffer separate
 * from the RGBA scene surface. */
static int8_t *lighting_scene_depth;
static int16_t *lighting_scene_sample_y;
static uint8_t *lighting_scene_marked;
static uint32_t *lighting_scene_pixels;
static size_t lighting_scene_pixels_num;
static size_t lighting_scene_pixels_capacity;
static int lighting_scene_width;
static int lighting_scene_height;

typedef struct lighting_scene_visibility {
    SDL_Surface *source;
    uint8_t *pixels;
    size_t pixels_num;
    const void *source_pixels;
    int source_width;
    int source_height;
    Uint32 source_format;
    bool invalidated;
    UT_hash_handle hh;
} lighting_scene_visibility;

static lighting_scene_visibility *lighting_scene_visibility_cache;

#define LIGHTING_SCENE_SAMPLE_Y_RAW INT16_MIN

#define LIGHTING_SCENE_INLINE __attribute__((always_inline)) inline

static LIGHTING_SCENE_INLINE uint16_t lighting_scene_neutral_linear(uint16_t radiance) {
    if (radiance > 2048) {
        return UINT16_MAX;
    }

    uint32_t lower;
    uint32_t low_level;
    uint32_t level_range;
    uint32_t shift;
    if (radiance <= 32) {
        lower = 0;
        low_level = 0;
        level_range = 45;
        shift = 5;
    } else if (radiance <= 64) {
        lower = 32;
        low_level = 45;
        level_range = 35;
        shift = 5;
    } else if (radiance <= 128) {
        lower = 64;
        low_level = 80;
        level_range = 40;
        shift = 6;
    } else if (radiance <= 256) {
        lower = 128;
        low_level = 120;
        level_range = 45;
        shift = 7;
    } else if (radiance <= 512) {
        lower = 256;
        low_level = 165;
        level_range = 50;
        shift = 8;
    } else if (radiance <= 1024) {
        lower = 512;
        low_level = 215;
        level_range = 30;
        shift = 9;
    } else {
        lower = 1024;
        low_level = 245;
        level_range = 10;
        shift = 10;
    }
    uint32_t range = UINT32_C(1) << shift;
    uint32_t numerator = (low_level << shift) + (radiance - lower) * level_range;
    uint32_t code = numerator >> shift;
    uint32_t remainder = numerator & (range - 1);
    if (code >= UINT8_MAX || remainder == 0) {
        return lighting_srgb8_to_linear_q16_lut[code];
    }
    uint32_t low = lighting_srgb8_to_linear_q16_lut[code];
    uint32_t high = lighting_srgb8_to_linear_q16_lut[code + 1];
    return (uint16_t)(low + (remainder * (high - low) + range / 2) / range);
}

static LIGHTING_SCENE_INLINE uint8_t lighting_scene_multiply(uint8_t source,
                                                              uint16_t illumination) {
    uint32_t source_linear = lighting_srgb8_to_linear_q16_lut[source];
    uint32_t product =
        (source_linear * (uint32_t)illumination + UINT16_MAX / 2) / UINT16_MAX;
    return lighting_linear_q16_to_srgb8_lut[product];
}

static void lighting_scene_visibility_clear(void) {
    lighting_scene_visibility *entry, *next;
    HASH_ITER(hh, lighting_scene_visibility_cache, entry, next) {
        HASH_DEL(lighting_scene_visibility_cache, entry);
        free(entry->pixels);
        free(entry);
    }
}

static void lighting_scene_visibility_clear_invalidated(void) {
    lighting_scene_visibility *entry, *next;
    HASH_ITER(hh, lighting_scene_visibility_cache, entry, next) {
        if (!entry->invalidated) {
            continue;
        }
        HASH_DEL(lighting_scene_visibility_cache, entry);
        free(entry->pixels);
        free(entry);
    }
}

static lighting_scene_visibility *lighting_scene_visibility_get(SDL_Surface *source) {
    lighting_scene_visibility *entry = NULL;
    Uint32 colorkey = 0;
    bool has_colorkey = SDL_GetSurfaceColorKey(source, &colorkey);
    HASH_FIND_PTR(lighting_scene_visibility_cache, &source, entry);
    if (entry != NULL) {
        if (!entry->invalidated && entry->source_pixels == source->pixels &&
            entry->source_width == source->w &&
            entry->source_height == source->h &&
            entry->source_format == source->format) {
            return entry;
        }
        HASH_DEL(lighting_scene_visibility_cache, entry);
        free(entry->pixels);
        free(entry);
        entry = NULL;
    }
    if (source->w <= 0 || source->h <= 0 ||
        (size_t)source->w > SIZE_MAX / (size_t)source->h) {
        return NULL;
    }
    size_t pixels_num = (size_t)source->w * (size_t)source->h;
    entry = calloc(1, sizeof(*entry));
    if (entry == NULL || pixels_num > SIZE_MAX / sizeof(*entry->pixels)) {
        free(entry);
        return NULL;
    }
    entry->pixels = calloc(pixels_num, sizeof(*entry->pixels));
    if (entry->pixels == NULL) {
        free(entry);
        return NULL;
    }
    entry->source = source;
    entry->pixels_num = pixels_num;

    bool locked = false;
    if (SDL_MUSTLOCK(source)) {
        if (!SDL_LockSurface(source)) {
            free(entry->pixels);
            free(entry);
            return NULL;
        }
        locked = true;
    }
    const SDL_PixelFormatDetails *format = SDL_GetPixelFormatDetails(source->format);
    SDL_Palette *palette = SDL_GetSurfacePalette(source);
    if (format == NULL) {
        if (locked) {
            SDL_UnlockSurface(source);
        }
        free(entry->pixels);
        free(entry);
        return NULL;
    }
    for (int y = 0; y < source->h; y++) {
        for (int x = 0; x < source->w; x++) {
            Uint32 pixel;
            if (format->bytes_per_pixel == sizeof(Uint32)) {
                const Uint32 *row = (const Uint32 *)((const Uint8 *)source->pixels +
                                                     y * source->pitch);
                pixel = row[x];
            } else {
                pixel = getpixel(source, x, y);
            }
            if (has_colorkey && pixel == colorkey) {
                continue;
            }
            uint8_t alpha = SDL_ALPHA_OPAQUE;
            if (format->bytes_per_pixel == sizeof(Uint32) && format->Amask != 0) {
                alpha = (uint8_t)((pixel & format->Amask) >> format->Ashift);
            } else if (palette != NULL) {
                uint8_t red, green, blue;
                SDL_GetRGBA(pixel, format, palette, &red, &green, &blue, &alpha);
            }
            entry->pixels[(size_t)y * (size_t)source->w + (size_t)x] = alpha != 0;
        }
    }
    if (locked) {
        SDL_UnlockSurface(source);
    }
    entry->source_pixels = source->pixels;
    entry->source_width = source->w;
    entry->source_height = source->h;
    entry->source_format = source->format;
    HASH_ADD_PTR(lighting_scene_visibility_cache, source, entry);
    return entry;
}

static void lighting_sprite_cache_clear(lighting_context *context,
                                        lighting_sprite_invalidation_cause_t cause);

#define LIGHT_STRUCTURE_BLUR_RADIUS 24

#define LIGHTING_BENCHMARK_INCREMENT(_context_, _field_)  \
    do {                                                  \
        (_context_)->benchmark_counters._field_++;        \
        lighting_benchmark_statistics.counters._field_++; \
    } while (0)

#define LIGHTING_BENCHMARK_ADD(_context_, _field_, _value_)                    \
    do {                                                                       \
        (_context_)->benchmark_counters._field_ += (uint64_t)(_value_);        \
        lighting_benchmark_statistics.counters._field_ += (uint64_t)(_value_); \
    } while (0)

#define LIGHTING_BENCHMARK_MODE_INCREMENT(_context_, _mode_, _field_)                  \
    do {                                                                               \
        if ((_mode_) == LIGHTING_SURFACE_STRUCTURE) {                                  \
            LIGHTING_BENCHMARK_INCREMENT((_context_), lit_sprite_structure_##_field_); \
        } else {                                                                       \
            LIGHTING_BENCHMARK_INCREMENT((_context_), lit_sprite_projected_##_field_); \
        }                                                                              \
    } while (0)

static uint64_t lighting_benchmark_timing_start(void) {
    return lighting_benchmark_timing_enabled ? SDL_GetTicksNS() : 0;
}

#define LIGHTING_BENCHMARK_TIMING_FINISH(_context_, _field_, _started_)          \
    do {                                                                         \
        if ((_started_) != 0) {                                                  \
            uint64_t elapsed = SDL_GetTicksNS() - (_started_);                   \
            (_context_)->benchmark_timings._field_.calls++;                      \
            (_context_)->benchmark_timings._field_.elapsed_ns += elapsed;        \
            lighting_benchmark_statistics.timings._field_.calls++;               \
            lighting_benchmark_statistics.timings._field_.elapsed_ns += elapsed; \
        }                                                                        \
    } while (0)

static size_t lighting_context_semantic_field_bytes(const lighting_context *context) {
    return context->samples_num *
               (sizeof(*context->samples) + sizeof(*context->structure_samples)) +
           (size_t)context->width * sizeof(*context->structure_blur_row) +
           (size_t)context->height * sizeof(*context->rows_valid);
}

static size_t lighting_context_retained_field_bytes(const lighting_context *context) {
    return lighting_context_semantic_field_bytes(context) +
           context->samples_num *
               (sizeof(*context->structure_illumination) +
                sizeof(*context->structure_illumination_valid)) +
           (size_t)context->height *
               (LIGHTING_MAX_DIRTY_SPANS * sizeof(*context->dirty_spans) +
                sizeof(*context->dirty_span_num));
}

static bool lighting_context_allocated(const lighting_context *context) {
    return context->samples != NULL;
}

static size_t lighting_context_sprite_entries(const lighting_context *context) {
    return (size_t)HASH_COUNT(context->sprite_cache);
}

/** Sort a small bounded integer list without relying on libc qsort state. */
static void lighting_sort_ints(int *values, size_t count) {
    for (size_t i = 1; i < count; i++) {
        int value = values[i];
        size_t position = i;
        while (position > 0 && values[position - 1] > value) {
            values[position] = values[position - 1];
            position--;
        }
        values[position] = value;
    }
}

static size_t lighting_dirty_pixels(const lighting_context *context) {
    int x_edges[LIGHTING_MAX_DIRTY_RECTS * 2];
    size_t x_edges_num = 0;
    for (size_t i = 0; i < context->dirty_num; i++) {
        x_edges[x_edges_num++] = context->dirty[i].x0;
        x_edges[x_edges_num++] = context->dirty[i].x1;
    }
    lighting_sort_ints(x_edges, x_edges_num);
    size_t unique_x_edges = 0;
    for (size_t i = 0; i < x_edges_num; i++) {
        if (unique_x_edges == 0 || x_edges[i] != x_edges[unique_x_edges - 1]) {
            x_edges[unique_x_edges++] = x_edges[i];
        }
    }

    size_t pixels = 0;
    for (size_t x_index = 0; x_index + 1 < unique_x_edges; x_index++) {
        int x0 = x_edges[x_index];
        int x1 = x_edges[x_index + 1];
        if (x0 >= x1) {
            continue;
        }

        lighting_interval_t intervals[LIGHTING_MAX_DIRTY_RECTS];
        size_t interval_num = 0;
        for (size_t rect_index = 0; rect_index < context->dirty_num; rect_index++) {
            const lighting_dirty_rect_t *rect = &context->dirty[rect_index];
            if (rect->x0 < x1 && rect->x1 > x0) {
                lighting_interval_t interval = {.start = rect->y0, .end = rect->y1};
                size_t position = interval_num;
                while (position > 0 && intervals[position - 1].start > interval.start) {
                    intervals[position] = intervals[position - 1];
                    position--;
                }
                intervals[position] = interval;
                interval_num++;
            }
        }

        int covered_y = 0;
        int current_y1 = 0;
        for (size_t interval_index = 0; interval_index < interval_num; interval_index++) {
            int y0 = intervals[interval_index].start;
            int y1 = intervals[interval_index].end;
            if (interval_index == 0) {
                covered_y = y1 - y0;
                current_y1 = y1;
                continue;
            }
            if (y0 > current_y1) {
                pixels += (size_t)(x1 - x0) * (size_t)covered_y;
                covered_y = y1 - y0;
            } else if (y1 > current_y1) {
                covered_y += y1 - current_y1;
            }
            current_y1 = MAX(current_y1, y1);
        }
        pixels += (size_t)(x1 - x0) * (size_t)covered_y;
    }
    return pixels;
}

/** Build non-overlapping horizontal spans from the bounded dirty rectangle union. */
static void lighting_dirty_spans_build(lighting_context *context) {
    context->dirty_span_overflow = false;
    for (int y = 0; y < context->height; y++) {
        lighting_dirty_span_t spans[LIGHTING_MAX_DIRTY_RECTS];
        size_t span_num = 0;
        for (size_t rect_index = 0; rect_index < context->dirty_num; rect_index++) {
            const lighting_dirty_rect_t *rect = &context->dirty[rect_index];
            if (y < rect->y0 || y >= rect->y1 || rect->x0 >= rect->x1) {
                continue;
            }

            lighting_dirty_span_t span = {.x0 = rect->x0, .x1 = rect->x1};
            size_t position = span_num;
            while (position > 0 && spans[position - 1].x0 > span.x0) {
                spans[position] = spans[position - 1];
                position--;
            }
            spans[position] = span;
            span_num++;
        }

        size_t merged_num = 0;
        for (size_t span_index = 0; span_index < span_num; span_index++) {
            lighting_dirty_span_t span = spans[span_index];
            if (merged_num != 0 && span.x0 <= spans[merged_num - 1].x1) {
                spans[merged_num - 1].x1 = MAX(spans[merged_num - 1].x1, span.x1);
                continue;
            }
            spans[merged_num++] = span;
        }

        if (merged_num > LIGHTING_MAX_DIRTY_SPANS) {
            context->dirty_span_overflow = true;
            context->dirty_span_num[y] = 0;
            continue;
        }
        context->dirty_span_num[y] = (uint8_t)merged_num;
        memcpy(context->dirty_spans +
                   (size_t)y * LIGHTING_MAX_DIRTY_SPANS,
               spans,
               merged_num * sizeof(*spans));
    }
}

static bool
lighting_rect_intersects(const lighting_context *context, int x0, int y0, int x1, int y1) {
    for (size_t i = 0; i < context->dirty_num; i++) {
        const lighting_dirty_rect_t *rect = &context->dirty[i];
        if (x0 < rect->x1 && x1 > rect->x0 && y0 < rect->y1 && y1 > rect->y0) {
            return true;
        }
    }
    return false;
}

static void lighting_dirty_full(lighting_context *context, lighting_full_rebuild_cause_t cause) {
    context->dirty[0] = (lighting_dirty_rect_t){0, 0, context->width, context->height};
    context->dirty_num = 1;
    if (cause != LIGHTING_FULL_REBUILD_CACHE ||
        context->full_rebuild_cause == LIGHTING_FULL_REBUILD_NONE) {
        context->full_rebuild_cause = cause;
    }
}

/** Hash one integer in a platform-independent byte order. */
static uint64_t lighting_benchmark_hash_uint64(uint64_t hash, uint64_t value) {
    for (size_t i = 0; i < sizeof(value); i++) {
        hash ^= (uint8_t)value;
        hash *= UINT64_C(1099511628211);
        value >>= 8;
    }
    return hash;
}

/** Hash stable state only; process-local surface addresses are excluded. */
static uint64_t lighting_context_state_digest(const lighting_context *context, int depth) {
    uint64_t hash = UINT64_C(14695981039346656037);
    hash = lighting_benchmark_hash_uint64(hash, (uint64_t)(int64_t)depth);
    hash = lighting_benchmark_hash_uint64(hash, lighting_context_allocated(context));
    hash = lighting_benchmark_hash_uint64(hash, context->active);
    hash = lighting_benchmark_hash_uint64(hash, context->cache_valid);
    hash = lighting_benchmark_hash_uint64(hash, context->update_needed);
    hash = lighting_benchmark_hash_uint64(hash, (uint64_t)(int64_t)context->width);
    hash = lighting_benchmark_hash_uint64(hash, (uint64_t)(int64_t)context->height);
    hash = lighting_benchmark_hash_uint64(hash, context->cache_key);
    hash = lighting_benchmark_hash_uint64(hash, context->pending_cache_key);
    hash = lighting_benchmark_hash_uint64(hash, lighting_context_sprite_entries(context));
    hash = lighting_benchmark_hash_uint64(hash, context->sprite_cache_bytes);
    /* Auxiliary derived caches must not change the visual-state digest. */
    hash = lighting_benchmark_hash_uint64(hash, lighting_context_semantic_field_bytes(context));

    uint64_t cache_xor = 0;
    uint64_t cache_sum = 0;
    lighting_sprite_cache_entry *entry, *next;
    HASH_ITER(hh, context->sprite_cache, entry, next) {
        uint64_t entry_hash = UINT64_C(14695981039346656037);
        entry_hash = lighting_benchmark_hash_uint64(entry_hash, (uint32_t)entry->key.source_x);
        entry_hash = lighting_benchmark_hash_uint64(entry_hash, (uint32_t)entry->key.source_y);
        entry_hash = lighting_benchmark_hash_uint64(entry_hash, (uint32_t)entry->key.source_w);
        entry_hash = lighting_benchmark_hash_uint64(entry_hash, (uint32_t)entry->key.source_h);
        entry_hash = lighting_benchmark_hash_uint64(entry_hash, entry->key.illumination_signature);
        entry_hash = lighting_benchmark_hash_uint64(entry_hash, entry->key.mode);
        entry_hash = lighting_benchmark_hash_uint64(entry_hash, entry->key.surface_alpha);
        entry_hash = lighting_benchmark_hash_uint64(entry_hash, entry->bytes);
        cache_xor ^= entry_hash;
        cache_sum += entry_hash;
    }
    hash = lighting_benchmark_hash_uint64(hash, cache_xor);
    hash = lighting_benchmark_hash_uint64(hash, cache_sum);
    return hash;
}

/** Refresh current peaks after a cache or field-state transition. */
static void lighting_benchmark_peaks_update(void) {
    size_t allocated = 0;
    size_t active = 0;
    size_t valid = 0;
    size_t dirty = 0;
    size_t sprite_entries = 0;
    size_t sprite_bytes = 0;
    size_t retained_bytes = 0;

    for (size_t i = 0; i < arraysize(lighting_contexts); i++) {
        lighting_context *context = &lighting_contexts[i];
        size_t context_entries = lighting_context_sprite_entries(context);
        size_t context_retained = lighting_context_retained_field_bytes(context);
        context->benchmark_peak_sprite_entries =
            MAX(context->benchmark_peak_sprite_entries, context_entries);
        context->benchmark_peak_sprite_bytes =
            MAX(context->benchmark_peak_sprite_bytes, context->sprite_cache_bytes);
        context->benchmark_peak_retained_field_bytes =
            MAX(context->benchmark_peak_retained_field_bytes, context_retained);
        allocated += lighting_context_allocated(context);
        active += context->active;
        valid += context->cache_valid;
        dirty += context->update_needed;
        sprite_entries += context_entries;
        sprite_bytes += context->sprite_cache_bytes;
        retained_bytes += context_retained;
    }

    lighting_benchmark_statistics.peak_allocated_levels =
        MAX(lighting_benchmark_statistics.peak_allocated_levels, allocated);
    lighting_benchmark_statistics.peak_active_levels =
        MAX(lighting_benchmark_statistics.peak_active_levels, active);
    lighting_benchmark_statistics.peak_cache_valid_levels =
        MAX(lighting_benchmark_statistics.peak_cache_valid_levels, valid);
    lighting_benchmark_statistics.peak_dirty_levels =
        MAX(lighting_benchmark_statistics.peak_dirty_levels, dirty);
    lighting_benchmark_statistics.peak_lit_sprite_entries =
        MAX(lighting_benchmark_statistics.peak_lit_sprite_entries, sprite_entries);
    lighting_benchmark_statistics.peak_lit_sprite_bytes =
        MAX(lighting_benchmark_statistics.peak_lit_sprite_bytes, sprite_bytes);
    lighting_benchmark_statistics.peak_retained_field_bytes =
        MAX(lighting_benchmark_statistics.peak_retained_field_bytes, retained_bytes);
}

static void lighting_context_free(lighting_context *context) {
    free(context->samples);
    free(context->structure_samples);
    free(context->structure_illumination);
    free(context->structure_blur_row);
    free(context->rows_valid);
    free(context->structure_illumination_valid);
    free(context->dirty_spans);
    free(context->dirty_span_num);
    lighting_sprite_cache_clear(context, LIGHTING_SPRITE_INVALIDATION_RESET);
    lighting_benchmark_timings_t benchmark_timings = context->benchmark_timings;
    memset(context, 0, sizeof(*context));
    if (lighting_benchmark_timing_enabled) {
        context->benchmark_timings = benchmark_timings;
    }
}

#define light_samples (lighting_context_current->samples)
#define structure_samples (lighting_context_current->structure_samples)
#define structure_illumination_cache (lighting_context_current->structure_illumination)
#define structure_blur_row (lighting_context_current->structure_blur_row)
#define structure_rows_valid (lighting_context_current->rows_valid)
#define lighting_dirty_spans (lighting_context_current->dirty_spans)
#define lighting_dirty_span_num (lighting_context_current->dirty_span_num)
#define structure_illumination_cache_valid \
    (lighting_context_current->structure_illumination_valid)
#define light_samples_num (lighting_context_current->samples_num)
#define lighting_width (lighting_context_current->width)
#define lighting_height (lighting_context_current->height)
#define lighting_active (lighting_context_current->active)
#define lighting_cache_valid (lighting_context_current->cache_valid)
#define lighting_update_needed (lighting_context_current->update_needed)
#define lighting_cache_key (lighting_context_current->cache_key)
#define lighting_pending_cache_key (lighting_context_current->pending_cache_key)

/** Convert one valid logical screen coordinate to its circular-field column. */
static inline int lighting_sample_physical_x(const lighting_context *context, int x) {
    int physical_x = x + context->sample_origin_x;
    if (physical_x >= context->width) {
        physical_x -= context->width;
    }
    return physical_x;
}

/** Return the physical row containing one logical screen row. */
static inline lighting_sample *lighting_sample_row_at(lighting_context *context, int y) {
    int physical_y = y + context->sample_origin_y;
    if (physical_y >= context->height) {
        physical_y -= context->height;
    }
    return context->samples + (size_t)physical_y * (size_t)context->width;
}

/** Return the physical row containing one logical screen row for a const field. */
static inline const lighting_sample *lighting_sample_row_at_const(const lighting_context *context,
                                                                  int y) {
    int physical_y = y + context->sample_origin_y;
    if (physical_y >= context->height) {
        physical_y -= context->height;
    }
    return context->samples + (size_t)physical_y * (size_t)context->width;
}

/** Return one sample using logical screen coordinates. */
static inline lighting_sample *lighting_sample_at(lighting_context *context, int x, int y) {
    return lighting_sample_row_at(context, y) + lighting_sample_physical_x(context, x);
}

/** Return one sample using logical coordinates for a const field. */
static inline const lighting_sample *lighting_sample_at_const(const lighting_context *context,
                                                               int x,
                                                               int y) {
    return lighting_sample_row_at_const(context, y) + lighting_sample_physical_x(context, x);
}

/** Clear a logical rectangle, splitting rows when the circular field wraps. */
static void lighting_clear_context_rect(lighting_context *context,
                                        const lighting_dirty_rect_t *rect) {
    for (int y = rect->y0; y < rect->y1; y++) {
        lighting_sample *row = lighting_sample_row_at(context, y);
        int x = rect->x0;
        while (x < rect->x1) {
            int physical_x = x + context->sample_origin_x;
            if (physical_x >= context->width) {
                physical_x -= context->width;
            }
            int length = MIN(rect->x1 - x, context->width - physical_x);
            memset(row + physical_x, 0, (size_t)length * sizeof(*row));
            x += length;
        }
    }
}

_Static_assert(sizeof(lighting_sprite_cache_entry) + sizeof(SDL_Surface) <=
                   LIGHTING_SPRITE_CACHE_ENTRY_OVERHEAD,
               "lit-sprite cache entry accounting overhead is too small");

static uint16_t lighting_sample_channel(const lighting_sample *sample, size_t channel) {
    HARD_ASSERT(sample != NULL);
    switch (channel) {
        case 0:
            return sample->scalar;
        case 1:
            return sample->red;
        case 2:
            return sample->green;
        case 3:
            return sample->blue;
        default:
            HARD_ASSERT(false);
            return 0;
    }
}

static void lighting_sample_channel_set(lighting_sample *sample, size_t channel, uint16_t value) {
    HARD_ASSERT(sample != NULL);
    sample->reserved = 0;
    switch (channel) {
        case 0:
            sample->scalar = value;
            break;
        case 1:
            sample->red = value;
            break;
        case 2:
            sample->green = value;
            break;
        case 3:
            sample->blue = value;
            break;
        default:
            HARD_ASSERT(false);
    }
}

/** Record why one cached result stopped being eligible for reuse. */
static void lighting_sprite_cache_invalidation_record(lighting_context *context,
                                                      const lighting_sprite_cache_entry *entry,
                                                      lighting_sprite_invalidation_cause_t cause) {
    LIGHTING_BENCHMARK_MODE_INCREMENT(context, entry->key.mode, invalidations);
    switch (cause) {
        case LIGHTING_SPRITE_INVALIDATION_FIELD:
            LIGHTING_BENCHMARK_INCREMENT(context, lit_sprite_invalidation_field);
            break;
        case LIGHTING_SPRITE_INVALIDATION_SCROLL:
            LIGHTING_BENCHMARK_INCREMENT(context, lit_sprite_invalidation_scroll);
            break;
        case LIGHTING_SPRITE_INVALIDATION_SOURCE:
            LIGHTING_BENCHMARK_INCREMENT(context, lit_sprite_invalidation_source);
            break;
        case LIGHTING_SPRITE_INVALIDATION_RESET:
            LIGHTING_BENCHMARK_INCREMENT(context, lit_sprite_invalidation_reset);
            break;
        case LIGHTING_SPRITE_INVALIDATION_EVICTION:
            LIGHTING_BENCHMARK_INCREMENT(context, lit_sprite_invalidation_eviction);
            break;
    }
}

/** Unlink and destroy one cache entry while preserving the LRU invariants. */
static void lighting_sprite_cache_remove(lighting_context *context,
                                         lighting_sprite_cache_entry *entry,
                                         lighting_sprite_invalidation_cause_t cause) {
    if (entry->lru_previous != NULL) {
        entry->lru_previous->lru_next = entry->lru_next;
    } else {
        context->sprite_cache_lru_oldest = entry->lru_next;
    }
    if (entry->lru_next != NULL) {
        entry->lru_next->lru_previous = entry->lru_previous;
    } else {
        context->sprite_cache_lru_newest = entry->lru_previous;
    }
    HASH_DEL(context->sprite_cache, entry);
    context->sprite_cache_bytes -= entry->bytes;
    lighting_sprite_cache_invalidation_record(context, entry, cause);
    free(entry->illumination);
    free(entry->structure_column_bottom);
    SDL_DestroySurface(entry->surface);
    free(entry);
}

static void lighting_sprite_cache_clear(lighting_context *context,
                                        lighting_sprite_invalidation_cause_t cause) {
    uint64_t timing_started = lighting_benchmark_timing_start();
    size_t entries = lighting_context_sprite_entries(context);
    LIGHTING_BENCHMARK_INCREMENT(context, lit_sprite_clears);
    context->benchmark_counters.lit_sprite_cleared_entries += entries;
    lighting_benchmark_statistics.counters.lit_sprite_cleared_entries += entries;
    lighting_sprite_cache_entry *entry, *next;
    HASH_ITER(hh, context->sprite_cache, entry, next) {
        lighting_sprite_cache_remove(context, entry, cause);
    }

    HARD_ASSERT(context->sprite_cache_bytes == 0);
    context->sprite_cache_lru_oldest = NULL;
    context->sprite_cache_lru_newest = NULL;
    LIGHTING_BENCHMARK_TIMING_FINISH(context, sprite_invalidation, timing_started);
}

/** Append an unlinked entry to the newest end of the cache's LRU list. */
static void lighting_sprite_cache_append(lighting_context *context,
                                         lighting_sprite_cache_entry *entry) {
    entry->lru_previous = context->sprite_cache_lru_newest;
    entry->lru_next = NULL;
    if (context->sprite_cache_lru_newest != NULL) {
        context->sprite_cache_lru_newest->lru_next = entry;
    } else {
        context->sprite_cache_lru_oldest = entry;
    }
    context->sprite_cache_lru_newest = entry;
}

/** Move an existing entry to the newest end of the cache's LRU list. */
static void lighting_sprite_cache_touch(lighting_context *context,
                                        lighting_sprite_cache_entry *entry) {
    if (context->sprite_cache_lru_newest == entry) {
        return;
    }

    if (entry->lru_previous != NULL) {
        entry->lru_previous->lru_next = entry->lru_next;
    } else {
        context->sprite_cache_lru_oldest = entry->lru_next;
    }

    HARD_ASSERT(entry->lru_next != NULL);
    entry->lru_next->lru_previous = entry->lru_previous;
    lighting_sprite_cache_append(context, entry);
}

/** Make room for one lit sprite without discarding the whole warm cache. */
static void lighting_sprite_cache_reserve(lighting_context *context, size_t bytes) {
    uint64_t timing_started = 0;
    while (context->sprite_cache != NULL &&
           (context->sprite_cache_bytes + bytes > LIGHTING_SPRITE_CACHE_MAX_BYTES ||
            HASH_COUNT(context->sprite_cache) >= LIGHTING_SPRITE_CACHE_MAX_ENTRIES)) {
        if (timing_started == 0) {
            timing_started = lighting_benchmark_timing_start();
        }
        lighting_sprite_cache_entry *oldest = context->sprite_cache_lru_oldest;
        HARD_ASSERT(oldest != NULL);
        LIGHTING_BENCHMARK_INCREMENT(context, lit_sprite_evictions);
        LIGHTING_BENCHMARK_MODE_INCREMENT(context, oldest->key.mode, evictions);
        lighting_sprite_cache_remove(context, oldest, LIGHTING_SPRITE_INVALIDATION_EVICTION);
    }
    LIGHTING_BENCHMARK_TIMING_FINISH(context, sprite_invalidation, timing_started);
}

bool lighting_select_level(int depth) {
    if (depth < -MAP2_MAX_DEPTH || depth > MAP2_MAX_DEPTH || lighting_active) {
        return false;
    }

    lighting_context_current = &lighting_contexts[MAP2_DEPTH_INDEX(depth)];
    return true;
}

void lighting_set_level_mask(uint16_t mask) {
    HARD_ASSERT(!lighting_active);

    for (size_t i = 0; i < arraysize(lighting_contexts); i++) {
        if (!(mask & (UINT16_C(1) << i))) {
            lighting_context_free(&lighting_contexts[i]);
        }
    }

    lighting_context_current = &lighting_contexts[MAP2_DEPTH_INDEX(0)];
}

void lighting_level_scroll(int dz) {
    HARD_ASSERT(!lighting_active);

    if (dz == 0) {
        return;
    }

    lighting_context shifted[MAP2_LEVELS] = {0};
    for (int depth = -MAP2_MAX_DEPTH; depth <= MAP2_MAX_DEPTH; depth++) {
        int source_depth = depth + dz;
        if (source_depth >= -MAP2_MAX_DEPTH && source_depth <= MAP2_MAX_DEPTH) {
            size_t destination = MAP2_DEPTH_INDEX(depth);
            size_t source = MAP2_DEPTH_INDEX(source_depth);
            shifted[destination] = lighting_contexts[source];
            memset(&lighting_contexts[source], 0, sizeof(lighting_contexts[source]));
        }
    }

    for (size_t i = 0; i < arraysize(lighting_contexts); i++) {
        lighting_context_free(&lighting_contexts[i]);
        lighting_contexts[i] = shifted[i];
    }

    lighting_context_current = &lighting_contexts[MAP2_DEPTH_INDEX(0)];
}

/** Create one 32-bit RGBA surface used by the software lighting pipeline. */
static SDL_Surface *lighting_rgba_surface_create(int width, int height) {
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
    const Uint32 rmask = 0xff000000;
    const Uint32 gmask = 0x00ff0000;
    const Uint32 bmask = 0x0000ff00;
    const Uint32 amask = 0x000000ff;
#else
    const Uint32 rmask = 0x000000ff;
    const Uint32 gmask = 0x0000ff00;
    const Uint32 bmask = 0x00ff0000;
    const Uint32 amask = 0xff000000;
#endif

    SDL_PixelFormat format = SDL_GetPixelFormatForMasks(32, rmask, gmask, bmask, amask);
    return SDL_CreateSurface(width, height, format);
}

static bool lighting_surface_create(int width, int height) {
    bool size_changed = lighting_width != width || lighting_height != height;
    if (size_changed) {
        size_t samples_num = (size_t)width * (size_t)height;
        light_samples = xreallocarray(light_samples, samples_num, sizeof(*light_samples));
        structure_samples =
            xreallocarray(structure_samples, samples_num, sizeof(*structure_samples));
        structure_illumination_cache = xreallocarray(structure_illumination_cache,
                                                     samples_num,
                                                     sizeof(*structure_illumination_cache));
        structure_blur_row =
            xreallocarray(structure_blur_row, (size_t)width, sizeof(*structure_blur_row));
        structure_rows_valid =
            xreallocarray(structure_rows_valid, (size_t)height, sizeof(*structure_rows_valid));
        structure_illumination_cache_valid = xreallocarray(structure_illumination_cache_valid,
                                                            samples_num,
                                                            sizeof(*structure_illumination_cache_valid));
        lighting_dirty_spans = xreallocarray(lighting_dirty_spans,
                                              (size_t)height * LIGHTING_MAX_DIRTY_SPANS,
                                              sizeof(*lighting_dirty_spans));
        lighting_dirty_span_num = xreallocarray(lighting_dirty_span_num,
                                                (size_t)height,
                                                sizeof(*lighting_dirty_span_num));
        memset(structure_illumination_cache_valid,
               0,
               samples_num * sizeof(*structure_illumination_cache_valid));
        light_samples_num = samples_num;
        lighting_width = width;
        lighting_height = height;
        lighting_context_current->sample_origin_x = 0;
        lighting_context_current->sample_origin_y = 0;
        lighting_cache_valid = false;
        lighting_dirty_full(lighting_context_current, LIGHTING_FULL_REBUILD_CACHE);
    }
    lighting_benchmark_peaks_update();
    return true;
}

/** Invalidate structural samples whose filtered field depends on a dirty area. */
static void lighting_structure_illumination_invalidate_rect(lighting_context *context,
                                                            int x0,
                                                            int y0,
                                                            int x1,
                                                            int y1) {
    if (context->structure_illumination_valid == NULL) {
        return;
    }

    int expanded_x0 = MAX(0, x0 - LIGHT_STRUCTURE_BLUR_RADIUS * 2);
    int expanded_y0 = MAX(0, y0 - LIGHT_STRUCTURE_BLUR_RADIUS);
    int expanded_x1 = MIN(context->width, x1 + LIGHT_STRUCTURE_BLUR_RADIUS * 2);
    int expanded_y1 = MIN(context->height, y1 + LIGHT_STRUCTURE_BLUR_RADIUS);
    for (int y = expanded_y0; y < expanded_y1; y++) {
        memset(context->structure_illumination_valid +
                   (size_t)y * (size_t)context->width + (size_t)expanded_x0,
               0,
               (size_t)(expanded_x1 - expanded_x0) *
                   sizeof(*context->structure_illumination_valid));
    }
}

/** Invalidate cached structural samples for the current lighting update. */
static void lighting_structure_illumination_invalidate_dirty(lighting_context *context,
                                                             bool full) {
    if (context->structure_illumination_valid == NULL) {
        return;
    }
    if (full) {
        memset(context->structure_illumination_valid,
               0,
               context->samples_num * sizeof(*context->structure_illumination_valid));
        return;
    }
    for (size_t i = 0; i < context->dirty_num; i++) {
        const lighting_dirty_rect_t *rect = &context->dirty[i];
        lighting_structure_illumination_invalidate_rect(
            context, rect->x0, rect->y0, rect->x1, rect->y1);
    }
}

bool lighting_begin(int width, int height, uint64_t cache_key) {
    HARD_ASSERT(width > 0);
    HARD_ASSERT(height > 0);
    HARD_ASSERT(!lighting_active);

    if (!lighting_surface_create(width, height)) {
        return false;
    }

    lighting_update_needed =
        lighting_update_needed || !lighting_cache_valid || lighting_cache_key != cache_key;
    if (!lighting_cache_valid || lighting_cache_key != cache_key) {
        lighting_dirty_full(lighting_context_current, LIGHTING_FULL_REBUILD_CACHE);
    }
    lighting_pending_cache_key = cache_key;
    LIGHTING_BENCHMARK_INCREMENT(lighting_context_current, field_begins);
    if (lighting_update_needed) {
        LIGHTING_BENCHMARK_INCREMENT(lighting_context_current, field_dirty_marks);
        lighting_dirty_spans_build(lighting_context_current);
        size_t dirty_pixels = lighting_dirty_pixels(lighting_context_current);
        if (dirty_pixels == light_samples_num &&
            lighting_context_current->full_rebuild_cause == LIGHTING_FULL_REBUILD_NONE) {
            /* A union of bounded invalidation rectangles can cover the full
             * field without going through lighting_dirty_full(). Keep the
             * benchmark cause accounting explicit for that case. */
            lighting_context_current->full_rebuild_cause = LIGHTING_FULL_REBUILD_BOUNDS;
        }
        lighting_context_current->benchmark_counters.field_dirty_pixels += dirty_pixels;
        lighting_benchmark_statistics.counters.field_dirty_pixels += dirty_pixels;
        uint64_t timing_started = lighting_benchmark_timing_start();
        for (size_t i = 0; i < lighting_context_current->dirty_num; i++) {
            lighting_clear_context_rect(lighting_context_current,
                                         &lighting_context_current->dirty[i]);
        }
        LIGHTING_BENCHMARK_TIMING_FINISH(lighting_context_current, dirty_clear, timing_started);
    } else {
        LIGHTING_BENCHMARK_INCREMENT(lighting_context_current, field_reuses);
    }

    lighting_active = true;
    lighting_benchmark_peaks_update();
    return true;
}

void lighting_clear_sprite_cache(void) {
    for (size_t i = 0; i < arraysize(lighting_contexts); i++) {
        lighting_sprite_cache_clear(&lighting_contexts[i], LIGHTING_SPRITE_INVALIDATION_RESET);
    }
    lighting_benchmark_peaks_update();
}

void lighting_invalidate_surface(SDL_Surface *source) {
    if (source == NULL) {
        return;
    }

    lighting_scene_visibility *visibility = NULL;
    HASH_FIND_PTR(lighting_scene_visibility_cache, &source, visibility);
    if (visibility != NULL) {
        visibility->invalidated = true;
    }

    for (size_t i = 0; i < arraysize(lighting_contexts); i++) {
        lighting_context *context = &lighting_contexts[i];
        uint64_t timing_started = 0;
        lighting_sprite_cache_entry *entry, *next;
        HASH_ITER(hh, context->sprite_cache, entry, next) {
            if (entry->key.source != source) {
                continue;
            }
            if (timing_started == 0) {
                timing_started = lighting_benchmark_timing_start();
            }
            lighting_sprite_cache_remove(context, entry, LIGHTING_SPRITE_INVALIDATION_SOURCE);
        }
        LIGHTING_BENCHMARK_TIMING_FINISH(context, sprite_invalidation, timing_started);
    }
    lighting_benchmark_peaks_update();
}

void lighting_benchmark_statistics_reset(void) {
    memset(&lighting_benchmark_statistics, 0, sizeof(lighting_benchmark_statistics));
    for (size_t i = 0; i < arraysize(lighting_contexts); i++) {
        lighting_context *context = &lighting_contexts[i];
        memset(&context->benchmark_counters, 0, sizeof(context->benchmark_counters));
        memset(&context->benchmark_timings, 0, sizeof(context->benchmark_timings));
        context->benchmark_peak_sprite_entries = lighting_context_sprite_entries(context);
        context->benchmark_peak_sprite_bytes = context->sprite_cache_bytes;
        context->benchmark_peak_retained_field_bytes =
            lighting_context_retained_field_bytes(context);
    }
    lighting_benchmark_peaks_update();
    lighting_benchmark_statistics_t current;
    lighting_benchmark_statistics_get(&current);
    lighting_benchmark_statistics.start_allocated_levels = current.allocated_levels;
    lighting_benchmark_statistics.start_active_levels = current.active_levels;
    lighting_benchmark_statistics.start_cache_valid_levels = current.cache_valid_levels;
    lighting_benchmark_statistics.start_dirty_levels = current.dirty_levels;
    lighting_benchmark_statistics.start_lit_sprite_entries = current.lit_sprite_entries;
    lighting_benchmark_statistics.start_lit_sprite_bytes = current.lit_sprite_bytes;
    lighting_benchmark_statistics.start_retained_field_bytes = current.retained_field_bytes;
    lighting_benchmark_statistics.start_state_digest = current.state_digest;
}

void lighting_benchmark_configure(bool timing_enabled,
                                  lighting_benchmark_reconstruction_t reconstruction) {
    lighting_benchmark_timing_enabled = timing_enabled;
    lighting_benchmark_reconstruction = reconstruction;
}

#ifdef ATRINIK_WIDGET_TESTS
void lighting_benchmark_fault_configure(unsigned int fault) {
    lighting_benchmark_fault_enabled = fault != 0;
    lighting_benchmark_fault_observed = 0;
    lighting_benchmark_fault_expected = fault == 0 ? 0 : (uint8_t)(UINT8_C(1) << (fault - 1));
}

bool lighting_benchmark_fault_complete(void) {
    if (lighting_benchmark_fault_expected == LIGHTING_BENCHMARK_FAULT_SOURCE_LIFETIME &&
        lighting_benchmark_fault_observed == 0) {
        if (!sprite_benchmark_source_lifetime_complete()) {
            return false;
        }
        lighting_benchmark_fault_observed = LIGHTING_BENCHMARK_FAULT_SOURCE_LIFETIME;
        return true;
    }
    return lighting_benchmark_fault_expected != 0 &&
           lighting_benchmark_fault_observed == lighting_benchmark_fault_expected &&
           lighting_benchmark_statistics.timings.sprite_construction.calls >= 1 &&
           lighting_benchmark_statistics.timings.sprite_construction.elapsed_ns > 0;
}

bool lighting_benchmark_source_address_retained(uintptr_t source_address) {
    for (size_t i = 0; i < arraysize(lighting_contexts); i++) {
        lighting_sprite_cache_entry *entry, *next;
        HASH_ITER(hh, lighting_contexts[i].sprite_cache, entry, next) {
            if ((uintptr_t)entry->key.source == source_address) {
                return true;
            }
        }
    }
    return false;
}

static bool lighting_benchmark_fault_take(uint8_t fault) {
    if (!lighting_benchmark_fault_enabled || lighting_benchmark_fault_expected != fault ||
        (lighting_benchmark_fault_observed & fault)) {
        return false;
    }
    lighting_benchmark_fault_observed |= fault;
    return true;
}
#endif

void lighting_benchmark_statistics_get(lighting_benchmark_statistics_t *statistics) {
    HARD_ASSERT(statistics != NULL);
    *statistics = lighting_benchmark_statistics;
    statistics->field_rebuilds = statistics->counters.field_rebuilds;
    statistics->field_reuses = statistics->counters.field_reuses;
    statistics->lit_sprite_lookups = statistics->counters.lit_sprite_lookups;
    statistics->lit_sprite_hits = statistics->counters.lit_sprite_hits;
    statistics->lit_sprite_misses = statistics->counters.lit_sprite_misses;
    statistics->lit_sprite_evictions = statistics->counters.lit_sprite_evictions;
    statistics->start_allocated_levels = lighting_benchmark_statistics.start_allocated_levels;
    statistics->start_active_levels = lighting_benchmark_statistics.start_active_levels;
    statistics->start_cache_valid_levels = lighting_benchmark_statistics.start_cache_valid_levels;
    statistics->start_dirty_levels = lighting_benchmark_statistics.start_dirty_levels;
    statistics->start_lit_sprite_entries = lighting_benchmark_statistics.start_lit_sprite_entries;
    statistics->start_lit_sprite_bytes = lighting_benchmark_statistics.start_lit_sprite_bytes;
    statistics->start_retained_field_bytes =
        lighting_benchmark_statistics.start_retained_field_bytes;
    statistics->start_state_digest = lighting_benchmark_statistics.start_state_digest;
    uint64_t state_digest = UINT64_C(14695981039346656037);
    for (size_t i = 0; i < arraysize(lighting_contexts); i++) {
        lighting_context *context = &lighting_contexts[i];
        statistics->allocated_levels += lighting_context_allocated(context);
        statistics->active_levels += context->active;
        statistics->cache_valid_levels += context->cache_valid;
        statistics->dirty_levels += context->update_needed;
        statistics->lit_sprite_entries += lighting_context_sprite_entries(context);
        statistics->lit_sprite_bytes += context->sprite_cache_bytes;
        statistics->retained_field_bytes += lighting_context_retained_field_bytes(context);
        state_digest = lighting_benchmark_hash_uint64(
            state_digest,
            lighting_context_state_digest(context, (int)i - MAP2_MAX_DEPTH));
    }
    statistics->state_digest = state_digest;
}

void lighting_benchmark_timings_get(lighting_benchmark_timings_t *timings) {
    HARD_ASSERT(timings != NULL);
    *timings = lighting_benchmark_statistics.timings;
}

bool lighting_benchmark_level_statistics_get(int depth,
                                             lighting_benchmark_level_statistics_t *statistics) {
    if (statistics == NULL || depth < -MAP2_MAX_DEPTH || depth > MAP2_MAX_DEPTH) {
        return false;
    }

    lighting_context *context = &lighting_contexts[MAP2_DEPTH_INDEX(depth)];
    *statistics = (lighting_benchmark_level_statistics_t){
        .counters = context->benchmark_counters,
        .timings = context->benchmark_timings,
        .depth = depth,
        .allocated = lighting_context_allocated(context),
        .active = context->active,
        .cache_valid = context->cache_valid,
        .update_needed = context->update_needed,
        .width = context->width,
        .height = context->height,
        .cache_key = context->cache_key,
        .pending_cache_key = context->pending_cache_key,
        .lit_sprite_entries = lighting_context_sprite_entries(context),
        .lit_sprite_bytes = context->sprite_cache_bytes,
        .retained_field_bytes = lighting_context_retained_field_bytes(context),
        .peak_lit_sprite_entries = context->benchmark_peak_sprite_entries,
        .peak_lit_sprite_bytes = context->benchmark_peak_sprite_bytes,
        .peak_retained_field_bytes = context->benchmark_peak_retained_field_bytes,
        .state_digest = lighting_context_state_digest(context, depth),
    };
    return true;
}

bool lighting_needs_update(void) {
    HARD_ASSERT(lighting_active);
    return lighting_update_needed;
}

bool lighting_viewport_size_get(int *width, int *height) {
    if (width == NULL || height == NULL) {
        return false;
    }

    const lighting_context *context = &lighting_contexts[MAP2_DEPTH_INDEX(0)];
    if (!lighting_context_allocated(context)) {
        return false;
    }

    *width = context->width;
    *height = context->height;
    return true;
}

/** Dirty a clipped viewport-relative rectangle in one allocated depth. */
void lighting_dirty_screen_rect(int depth, int x0, int y0, int x1, int y1) {
    if (depth < -MAP2_MAX_DEPTH || depth > MAP2_MAX_DEPTH || x0 >= x1 || y0 >= y1) {
        return;
    }

    lighting_context *context = &lighting_contexts[MAP2_DEPTH_INDEX(depth)];
    if (!lighting_context_allocated(context)) {
        return;
    }

    x0 += context->width / 2;
    y0 += context->height / 2 - depth * LIGHTING_MAP_LEVEL_PIXEL_HEIGHT;
    x1 += context->width / 2;
    y1 += context->height / 2 - depth * LIGHTING_MAP_LEVEL_PIXEL_HEIGHT;
    x0 = MAX(0, x0);
    y0 = MAX(0, y0);
    x1 = MIN(context->width, x1);
    y1 = MIN(context->height, y1);
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    if (context->dirty_num >= arraysize(context->dirty)) {
        lighting_dirty_full(context, LIGHTING_FULL_REBUILD_BOUNDS);
    } else {
        context->dirty[context->dirty_num++] = (lighting_dirty_rect_t){x0, y0, x1, y1};
    }
    context->update_needed = true;
}

/** Translate cached screen-space lighting and dirty changed scroll edges. */
void lighting_scroll(int screen_dx, int screen_dy) {
    if (screen_dx == 0 && screen_dy == 0) {
        return;
    }

    for (size_t i = 0; i < arraysize(lighting_contexts); i++) {
        lighting_context *context = &lighting_contexts[i];
        if (!lighting_context_allocated(context)) {
            continue;
        }
        if (context->active) {
            lighting_dirty_full(context, LIGHTING_FULL_REBUILD_ACTIVE);
            context->update_needed = true;
            LIGHTING_BENCHMARK_INCREMENT(context, field_translation_fallback_active);
            continue;
        }
        LIGHTING_BENCHMARK_ADD(context, field_scroll_x_pixels, abs(screen_dx));
        LIGHTING_BENCHMARK_ADD(context, field_scroll_y_pixels, abs(screen_dy));
        if (lighting_benchmark_reconstruction == LIGHTING_BENCHMARK_RECONSTRUCTION_FULL) {
            uint64_t timing_started = lighting_benchmark_timing_start();
            lighting_dirty_full(context, LIGHTING_FULL_REBUILD_CONTROL);
            memset(context->samples, 0, context->samples_num * sizeof(*context->samples));
            context->sample_origin_x = 0;
            context->sample_origin_y = 0;
            memset(context->rows_valid, 0, (size_t)context->height);
            memset(context->structure_illumination_valid,
                   0,
                   context->samples_num * sizeof(*context->structure_illumination_valid));
            context->update_needed = true;
            LIGHTING_BENCHMARK_INCREMENT(context, field_translation_fallback_control);
            LIGHTING_BENCHMARK_TIMING_FINISH(context, translation, timing_started);
            continue;
        }
        const int width = context->width;
        const int height = context->height;
        if (screen_dx <= -width || screen_dx >= width || screen_dy <= -height ||
            screen_dy >= height) {
            uint64_t timing_started = lighting_benchmark_timing_start();
            lighting_dirty_full(context, LIGHTING_FULL_REBUILD_BOUNDS);
            memset(context->samples, 0, context->samples_num * sizeof(*context->samples));
            context->sample_origin_x = 0;
            context->sample_origin_y = 0;
            memset(context->rows_valid, 0, (size_t)height);
            memset(context->structure_illumination_valid,
                   0,
                   context->samples_num * sizeof(*context->structure_illumination_valid));
            context->update_needed = true;
            LIGHTING_BENCHMARK_INCREMENT(context, field_translation_fallback_bounds);
            LIGHTING_BENCHMARK_TIMING_FINISH(context, translation, timing_started);
            continue;
        }
        int source_x0 = screen_dx < 0 ? -screen_dx : 0;
        int source_x1 = screen_dx > 0 ? width - screen_dx : width;
        int source_y0 = screen_dy < 0 ? -screen_dy : 0;
        int source_y1 = screen_dy > 0 ? height - screen_dy : height;

        uint64_t timing_started = lighting_benchmark_timing_start();
        context->sample_origin_x -= screen_dx;
        if (context->sample_origin_x < 0) {
            context->sample_origin_x += width;
        } else if (context->sample_origin_x >= width) {
            context->sample_origin_x -= width;
        }
        context->sample_origin_y -= screen_dy;
        if (context->sample_origin_y < 0) {
            context->sample_origin_y += height;
        } else if (context->sample_origin_y >= height) {
            context->sample_origin_y -= height;
        }
        if (screen_dx > 0) {
            lighting_clear_context_rect(context,
                                        &(lighting_dirty_rect_t){0, 0, screen_dx, height});
        } else if (screen_dx < 0) {
            lighting_clear_context_rect(context,
                                        &(lighting_dirty_rect_t){width + screen_dx,
                                                                 0,
                                                                 width,
                                                                 height});
        }
        if (screen_dy > 0) {
            lighting_clear_context_rect(context,
                                        &(lighting_dirty_rect_t){0, 0, width, screen_dy});
        } else if (screen_dy < 0) {
            lighting_clear_context_rect(context,
                                        &(lighting_dirty_rect_t){0,
                                                                 height + screen_dy,
                                                                 width,
                                                                 height});
        }

        /* A translated field is not guaranteed to be a pure screen translation;
         * rebuild derived structural samples after every scroll. */
        memset(context->structure_illumination_valid,
               0,
               context->samples_num * sizeof(*context->structure_illumination_valid));

        context->dirty_num = 0;
        if (screen_dx > 0) {
            context->dirty[context->dirty_num++] =
                (lighting_dirty_rect_t){0,
                                        0,
                                        MIN(width, screen_dx + LIGHTING_SCROLL_MARGIN),
                                        height};
        } else if (screen_dx < 0) {
            context->dirty[context->dirty_num++] =
                (lighting_dirty_rect_t){MAX(0, width + screen_dx - LIGHTING_SCROLL_MARGIN),
                                        0,
                                        width,
                                        height};
        }
        if (screen_dy > 0) {
            context->dirty[context->dirty_num++] =
                (lighting_dirty_rect_t){0,
                                        0,
                                        width,
                                        MIN(height, screen_dy + LIGHTING_SCROLL_MARGIN)};
        } else if (screen_dy < 0) {
            context->dirty[context->dirty_num++] =
                (lighting_dirty_rect_t){0,
                                        MAX(0, height + screen_dy - LIGHTING_SCROLL_MARGIN),
                                        width,
                                        height};
        }
        memset(context->rows_valid, 0, (size_t)context->height);
        context->update_needed = true;
        LIGHTING_BENCHMARK_INCREMENT(context, field_translations);
        size_t translated_pixels =
            (size_t)(source_x1 - source_x0) * (size_t)(source_y1 - source_y0);
        LIGHTING_BENCHMARK_ADD(context, field_translated_pixels, translated_pixels);
        LIGHTING_BENCHMARK_ADD(context,
                               field_translated_bytes,
                               translated_pixels * sizeof(*context->samples));
        LIGHTING_BENCHMARK_TIMING_FINISH(context, translation, timing_started);
    }
    lighting_benchmark_peaks_update();
}

/** Evaluate an edge at doubled coordinates, preserving pixel-center precision.
 */
static int64_t
lighting_edge(const lighting_vertex_t *a, const lighting_vertex_t *b, int x2, int y2) {
    return (int64_t)(x2 - a->x * 2) * (b->y - a->y) - (int64_t)(y2 - a->y * 2) * (b->x - a->x);
}

/** Rasterize one half of a quad while interpolating its four corner levels. */
static void lighting_draw_triangle(const lighting_vertex_t vertices[4], bool first_half) {
    lighting_context *context = lighting_context_current;
    const lighting_vertex_t *a = &vertices[0];
    const lighting_vertex_t *b = &vertices[first_half ? 1 : 2];
    const lighting_vertex_t *c = &vertices[first_half ? 2 : 3];
    int64_t area = lighting_edge(a, b, c->x * 2, c->y * 2);
    if (area == 0) {
        return;
    }

    int orientation = area < 0 ? -1 : 1;
    area *= orientation;
    if ((uint64_t)area > UINT64_MAX / (UINT64_C(1) + UINT16_MAX)) {
        LOG(ERROR, "Lighting quad exceeds the bounded interpolation range.");
        return;
    }

    int min_x = MAX(0, MIN(a->x, MIN(b->x, c->x)));
    int max_x = MIN(lighting_width - 1, MAX(a->x, MAX(b->x, c->x)));
    int min_y = MAX(0, MIN(a->y, MIN(b->y, c->y)));
    int max_y = MIN(lighting_height - 1, MAX(a->y, MAX(b->y, c->y)));
    if (!lighting_rect_intersects(lighting_context_current, min_x, min_y, max_x + 1, max_y + 1)) {
        return;
    }

    int64_t row_weight_a = orientation * lighting_edge(b, c, min_x * 2 + 1, min_y * 2 + 1);
    int64_t row_weight_b = orientation * lighting_edge(c, a, min_x * 2 + 1, min_y * 2 + 1);
    int64_t row_weight_c = orientation * lighting_edge(a, b, min_x * 2 + 1, min_y * 2 + 1);
    int64_t step_x_a = orientation * 2 * (c->y - b->y);
    int64_t step_x_b = orientation * 2 * (a->y - c->y);
    int64_t step_x_c = orientation * 2 * (b->y - a->y);
    int64_t step_y_a = orientation * -2 * (c->x - b->x);
    int64_t step_y_b = orientation * -2 * (a->x - c->x);
    int64_t step_y_c = orientation * -2 * (b->x - a->x);
    bool scalar_constant = vertices[0].scalar == vertices[1].scalar &&
                           vertices[0].scalar == vertices[2].scalar &&
                           vertices[0].scalar == vertices[3].scalar;
    bool red_constant = vertices[0].red == vertices[1].red &&
                        vertices[0].red == vertices[2].red &&
                        vertices[0].red == vertices[3].red;
    bool green_constant = vertices[0].green == vertices[1].green &&
                          vertices[0].green == vertices[2].green &&
                          vertices[0].green == vertices[3].green;
    bool blue_constant = vertices[0].blue == vertices[1].blue &&
                         vertices[0].blue == vertices[2].blue &&
                         vertices[0].blue == vertices[3].blue;
    bool neutral = vertices[0].scalar == vertices[0].red &&
                   vertices[0].scalar == vertices[0].green &&
                   vertices[0].scalar == vertices[0].blue &&
                   vertices[1].scalar == vertices[1].red &&
                   vertices[1].scalar == vertices[1].green &&
                   vertices[1].scalar == vertices[1].blue &&
                   vertices[2].scalar == vertices[2].red &&
                   vertices[2].scalar == vertices[2].green &&
                   vertices[2].scalar == vertices[2].blue &&
                   vertices[3].scalar == vertices[3].red &&
                   vertices[3].scalar == vertices[3].green &&
                   vertices[3].scalar == vertices[3].blue;

    for (int y = min_y; y <= max_y; y++) {
        lighting_dirty_span_t fallback_spans[LIGHTING_MAX_DIRTY_RECTS];
        const lighting_dirty_span_t *spans = NULL;
        size_t span_num = 0;
        if (lighting_context_current->dirty_span_overflow) {
            span_num = 0;
            for (size_t dirty_index = 0; dirty_index < lighting_context_current->dirty_num;
                 dirty_index++) {
                const lighting_dirty_rect_t *dirty =
                    &lighting_context_current->dirty[dirty_index];
                if (y >= dirty->y0 && y < dirty->y1) {
                    fallback_spans[span_num++] = (lighting_dirty_span_t){dirty->x0, dirty->x1};
                }
            }
            spans = fallback_spans;
        } else {
            spans = lighting_context_current->dirty_spans +
                    (size_t)y * LIGHTING_MAX_DIRTY_SPANS;
            span_num = lighting_context_current->dirty_span_num[y];
        }

        lighting_sample *row = lighting_sample_row_at(context, y);
        for (size_t span_index = 0; span_index < span_num; span_index++) {
            const lighting_dirty_span_t *span = &spans[span_index];
            int x_start = MAX(min_x, span->x0);
            int x_end = MIN(max_x, span->x1 - 1);
            if (x_start > x_end) {
                continue;
            }

            int64_t weight_a = row_weight_a + step_x_a * (x_start - min_x);
            int64_t weight_b = row_weight_b + step_x_b * (x_start - min_x);
            int64_t weight_c = row_weight_c + step_x_c * (x_start - min_x);
            int x = x_start;
            while (x <= x_end) {
                int physical_x = x + context->sample_origin_x;
                if (physical_x >= context->width) {
                    physical_x -= context->width;
                }
                int segment_end = MIN(x_end, x + context->width - physical_x - 1);
                lighting_sample *samples = row + physical_x;
                for (; x <= segment_end; x++, samples++) {
                    if (weight_a >= 0 && weight_b >= 0 && weight_c >= 0) {
                        /* Recover the quad's logical U/V coordinates from this
                         * triangle's barycentric weights, then blend all four light
                         * samples. This removes the visible gradient crease created
                         * by treating the two halves as unrelated planes. */
                        int64_t u = first_half ? weight_b + weight_c : weight_b;
                        int64_t v = first_half ? weight_c : weight_b + weight_c;
#define INTERPOLATE_CHANNEL(_channel_)               \
    lighting_bilinear_channel(vertices[0]._channel_, \
                              vertices[1]._channel_, \
                              vertices[2]._channel_, \
                              vertices[3]._channel_, \
                              (uint64_t)u,           \
                              (uint64_t)v,           \
                              (uint64_t)area)
                    uint16_t scalar = scalar_constant
                                          ? vertices[0].scalar
                                          : INTERPOLATE_CHANNEL(scalar);
                    samples->scalar = scalar;
                    if (neutral) {
                        samples->red = scalar;
                        samples->green = scalar;
                        samples->blue = scalar;
                    } else {
                        samples->red = red_constant ? vertices[0].red : INTERPOLATE_CHANNEL(red);
                        samples->green = green_constant ? vertices[0].green
                                                         : INTERPOLATE_CHANNEL(green);
                        samples->blue = blue_constant ? vertices[0].blue
                                                       : INTERPOLATE_CHANNEL(blue);
                    }
                    samples->present = 1;
                    samples->reserved = 0;
#undef INTERPOLATE_CHANNEL
                    }
                    weight_a += step_x_a;
                    weight_b += step_x_b;
                    weight_c += step_x_c;
                }
            }
        }

        row_weight_a += step_y_a;
        row_weight_b += step_y_b;
        row_weight_c += step_y_c;
    }
}

void lighting_draw_quad(const lighting_vertex_t vertices[4]) {
    HARD_ASSERT(vertices != NULL);
    HARD_ASSERT(light_samples != NULL);
    HARD_ASSERT(lighting_width > 0);
    HARD_ASSERT(lighting_height > 0);
    HARD_ASSERT(lighting_active);
    HARD_ASSERT(lighting_update_needed);

    int min_x = MIN(MIN(vertices[0].x, vertices[1].x), MIN(vertices[2].x, vertices[3].x));
    int max_x = MAX(MAX(vertices[0].x, vertices[1].x), MAX(vertices[2].x, vertices[3].x));
    int min_y = MIN(MIN(vertices[0].y, vertices[1].y), MIN(vertices[2].y, vertices[3].y));
    int max_y = MAX(MAX(vertices[0].y, vertices[1].y), MAX(vertices[2].y, vertices[3].y));
    if (max_x < 0 || min_x >= lighting_width || max_y < 0 || min_y >= lighting_height) {
        return;
    }

    /* Scroll and localized field updates expose only a bounded union of
     * pixels.  The triangle rasterizer repeats this test for each half, but
     * most map quads miss the union entirely.  Reject those quads before
     * taking benchmark timings or doing interpolation setup. */
    int clipped_min_x = MAX(0, min_x);
    int clipped_max_x = MIN(lighting_width - 1, max_x);
    int clipped_min_y = MAX(0, min_y);
    int clipped_max_y = MIN(lighting_height - 1, max_y);
    if (!lighting_rect_intersects(lighting_context_current,
                                  clipped_min_x,
                                  clipped_min_y,
                                  clipped_max_x + 1,
                                  clipped_max_y + 1)) {
        return;
    }

    uint64_t timing_started = lighting_benchmark_timing_start();
    LIGHTING_BENCHMARK_INCREMENT(lighting_context_current, field_rasterized_quads);
    lighting_draw_triangle(vertices, true);
    lighting_draw_triangle(vertices, false);
    LIGHTING_BENCHMARK_TIMING_FINISH(lighting_context_current, rasterization, timing_started);
}

/**
 * Extend the sampled map field through unsampled screen pixels.
 *
 * Ground masks can extend outside their owning diamond, and projected terrain
 * can leave gaps between sampled rows. Extrapolating the nearest edge samples
 * lights those pixels like the surrounding map without treating the unsampled
 * area as fully bright. Elevated sprites are drawn after this lightmap.
 */
static void lighting_extrapolate(void) {
    lighting_context *context = lighting_context_current;
    int first_sampled_row = -1;
    int previous_sampled_row = -1;

    for (int y = 0; y < lighting_height; y++) {
        int first_sample = -1;
        int previous_sample = -1;

        for (int x = 0; x < lighting_width; x++) {
            lighting_sample *sample = lighting_sample_at(context, x, y);
            if (!sample->present) {
                continue;
            }

            if (first_sample == -1) {
                first_sample = x;
                for (int fill_x = 0; fill_x < x; fill_x++) {
                    *lighting_sample_at(context, fill_x, y) = *sample;
                }
            } else if (x > previous_sample + 1) {
                int distance = x - previous_sample;

                for (int fill_x = previous_sample + 1; fill_x < x; fill_x++) {
                    for (size_t channel = 0; channel < 4; channel++) {
                        uint16_t first = lighting_sample_channel(
                            lighting_sample_at(context, previous_sample, y), channel);
                        uint16_t last = lighting_sample_channel(sample, channel);
                        lighting_sample_channel_set(
                            lighting_sample_at(context, fill_x, y),
                            channel,
                            (uint16_t)((int32_t)first + ((int32_t)last - first) *
                                                            (fill_x - previous_sample) / distance));
                    }
                    lighting_sample_at(context, fill_x, y)->present = 1;
                }
            }

            previous_sample = x;
        }

        if (first_sample == -1) {
            continue;
        }

        lighting_sample *last_sample = lighting_sample_at(context, previous_sample, y);
        for (int fill_x = previous_sample + 1; fill_x < lighting_width; fill_x++) {
            *lighting_sample_at(context, fill_x, y) = *last_sample;
        }

        if (first_sampled_row == -1) {
            first_sampled_row = y;
            for (int fill_y = 0; fill_y < y; fill_y++) {
                for (int fill_x = 0; fill_x < lighting_width; fill_x++) {
                    *lighting_sample_at(context, fill_x, fill_y) =
                        *lighting_sample_at(context, fill_x, y);
                }
            }
        } else if (y > previous_sampled_row + 1) {
            int distance = y - previous_sampled_row;

            for (int fill_y = previous_sampled_row + 1; fill_y < y; fill_y++) {
                for (int fill_x = 0; fill_x < lighting_width; fill_x++) {
                    const lighting_sample *last = lighting_sample_at_const(context, fill_x, y);
                    lighting_sample *destination = lighting_sample_at(context, fill_x, fill_y);
                    const lighting_sample *first_sample =
                        lighting_sample_at_const(context, fill_x, previous_sampled_row);
                    for (size_t channel = 0; channel < 4; channel++) {
                        uint16_t first_value = lighting_sample_channel(first_sample, channel);
                        uint16_t last_value = lighting_sample_channel(last, channel);
                        lighting_sample_channel_set(
                            destination,
                            channel,
                            (uint16_t)((int32_t)first_value + ((int32_t)last_value - first_value) *
                                                                  (fill_y - previous_sampled_row) /
                                                                  distance));
                    }
                    destination->present = 1;
                }
            }
        }

        previous_sampled_row = y;
    }

    if (first_sampled_row == -1) {
        for (int y = 0; y < lighting_height; y++) {
            for (int x = 0; x < lighting_width; x++) {
                *lighting_sample_at(context, x, y) = (lighting_sample){2048,
                                                                       2048,
                                                                       2048,
                                                                       2048,
                                                                       1,
                                                                       0};
            }
        }
        return;
    }

    for (int y = previous_sampled_row + 1; y < lighting_height; y++) {
        for (int x = 0; x < lighting_width; x++) {
            *lighting_sample_at(context, x, y) =
                *lighting_sample_at(context, x, previous_sampled_row);
        }
    }
}

/** Fill only newly exposed field regions from their nearest valid boundary. */
static void lighting_extrapolate_dirty(void) {
    lighting_context *context = lighting_context_current;
    for (size_t rect_index = 0; rect_index < lighting_context_current->dirty_num; rect_index++) {
        const lighting_dirty_rect_t rect = context->dirty[rect_index];
        for (int y = rect.y0; y < rect.y1; y++) {
            int previous = -1;
            lighting_sample *previous_sample = NULL;
            int x = rect.x0;
            while (x < rect.x1) {
                lighting_sample *row = lighting_sample_row_at(context, y);
                int physical_x = lighting_sample_physical_x(context, x);
                int segment_end = MIN(rect.x1, x + context->width - physical_x);
                lighting_sample *samples = row + physical_x;
                for (; x < segment_end; x++, samples++) {
                    lighting_sample *sample = samples;
                    if (!sample->present) {
                        continue;
                    }
                    if (previous == -1) {
                        for (int fill_x = rect.x0; fill_x < x; fill_x++) {
                            *lighting_sample_at(context, fill_x, y) = *sample;
                        }
                    } else if (x > previous + 1) {
                        int distance = x - previous;
                        for (int fill_x = previous + 1; fill_x < x; fill_x++) {
                            for (size_t channel = 0; channel < 4; channel++) {
                                uint16_t first =
                                    lighting_sample_channel(previous_sample, channel);
                                uint16_t last = lighting_sample_channel(sample, channel);
                                lighting_sample_channel_set(
                                    lighting_sample_at(context, fill_x, y),
                                    channel,
                                    (uint16_t)((int32_t)first + ((int32_t)last - first) *
                                                                    (fill_x - previous) /
                                                                    distance));
                            }
                            lighting_sample_at(context, fill_x, y)->present = 1;
                        }
                    }
                    previous = x;
                    previous_sample = sample;
                }
            }
            if (previous != -1) {
                const lighting_sample *last = previous_sample;
                for (int fill_x = MAX(rect.x0, previous + 1); fill_x < rect.x1; fill_x++) {
                    *lighting_sample_at(context, fill_x, y) = *last;
                }
            } else {
                int seed_x = rect.x0 > 0                ? rect.x0 - 1
                             : rect.x1 < lighting_width ? rect.x1
                             : rect.x0;
                const lighting_sample *seed_sample = lighting_sample_at_const(context, seed_x, y);
                for (int fill_x = rect.x0; fill_x < rect.x1; fill_x++) {
                    *lighting_sample_at(context, fill_x, y) = *seed_sample;
                }
            }
        }

        for (int x = rect.x0; x < rect.x1; x++) {
            int previous = -1;
            int physical_x = lighting_sample_physical_x(context, x);
            const lighting_sample *previous_sample = NULL;
            for (int y = rect.y0; y < rect.y1; y++) {
                lighting_sample *sample = lighting_sample_row_at(context, y) + physical_x;
                if (!sample->present) {
                    continue;
                }
                if (previous != -1 && y > previous + 1) {
                    int distance = y - previous;
                    const lighting_sample *first_sample =
                        lighting_sample_row_at_const(context, previous) + physical_x;
                    for (int fill_y = MAX(rect.y0, previous + 1); fill_y < MIN(rect.y1, y);
                         fill_y++) {
                        lighting_sample *destination =
                            lighting_sample_row_at(context, fill_y) + physical_x;
                        for (size_t channel = 0; channel < 4; channel++) {
                            uint16_t first = lighting_sample_channel(first_sample, channel);
                            uint16_t last = lighting_sample_channel(sample, channel);
                            lighting_sample_channel_set(
                                destination,
                                channel,
                                (uint16_t)((int32_t)first + ((int32_t)last - first) *
                                                                (fill_y - previous) / distance));
                        }
                        destination->present = 1;
                    }
                }
                previous = y;
                previous_sample = sample;
            }
            if (previous != -1) {
                const lighting_sample *last = previous_sample;
                for (int fill_y = MAX(rect.y0, previous + 1); fill_y < rect.y1; fill_y++) {
                    *(lighting_sample_row_at(context, fill_y) + physical_x) = *last;
                }
            } else {
                int seed_y = rect.y0 > 0                 ? rect.y0 - 1
                             : rect.y1 < lighting_height ? rect.y1
                                                         : rect.y0;
                const lighting_sample *seed_sample =
                    lighting_sample_row_at_const(context, seed_y) + physical_x;
                for (int fill_y = rect.y0; fill_y < rect.y1; fill_y++) {
                    *(lighting_sample_row_at(context, fill_y) + physical_x) = *seed_sample;
                }
            }
        }
    }
}

/** Apply one horizontal box-blur pass to a logical light sample row. */
static void lighting_blur_logical_row(int y, lighting_sample *destination) {
    const int radius = LIGHT_STRUCTURE_BLUR_RADIUS;
    const uint32_t diameter = radius * 2 + 1;
    uint32_t sum[4] = {0};

    for (int offset = -radius; offset <= radius; offset++) {
        int source_x = MAX(0, MIN(lighting_width - 1, offset));
        const lighting_sample *sample =
            lighting_sample_at_const(lighting_context_current, source_x, y);
        for (size_t channel = 0; channel < 4; channel++) {
            sum[channel] += lighting_sample_channel(sample, channel);
        }
    }

    for (int x = 0; x < lighting_width; x++) {
        for (size_t channel = 0; channel < 4; channel++) {
            lighting_sample_channel_set(&destination[x],
                                        channel,
                                        (uint16_t)((sum[channel] + diameter / 2) / diameter));
        }
        destination[x].present = 1;

        int outgoing_x = MAX(0, MIN(lighting_width - 1, x - radius));
        int incoming_x = MAX(0, MIN(lighting_width - 1, x + radius + 1));
        const lighting_sample *outgoing =
            lighting_sample_at_const(lighting_context_current, outgoing_x, y);
        const lighting_sample *incoming =
            lighting_sample_at_const(lighting_context_current, incoming_x, y);
        for (size_t channel = 0; channel < 4; channel++) {
            sum[channel] -= lighting_sample_channel(outgoing, channel);
            sum[channel] += lighting_sample_channel(incoming, channel);
        }
    }
}

/** Apply one horizontal box-blur pass to a contiguous light sample row. */
static void lighting_blur_contiguous_row(const lighting_sample *source,
                                          lighting_sample *destination) {
    const int radius = LIGHT_STRUCTURE_BLUR_RADIUS;
    const uint32_t diameter = radius * 2 + 1;
    uint32_t sum[4] = {0};

    for (int offset = -radius; offset <= radius; offset++) {
        int source_x = MAX(0, MIN(lighting_width - 1, offset));
        for (size_t channel = 0; channel < 4; channel++) {
            sum[channel] += lighting_sample_channel(&source[source_x], channel);
        }
    }

    for (int x = 0; x < lighting_width; x++) {
        for (size_t channel = 0; channel < 4; channel++) {
            lighting_sample_channel_set(&destination[x],
                                        channel,
                                        (uint16_t)((sum[channel] + diameter / 2) / diameter));
        }
        destination[x].present = 1;

        int outgoing_x = MAX(0, MIN(lighting_width - 1, x - radius));
        int incoming_x = MAX(0, MIN(lighting_width - 1, x + radius + 1));
        for (size_t channel = 0; channel < 4; channel++) {
            sum[channel] -= lighting_sample_channel(&source[outgoing_x], channel);
            sum[channel] += lighting_sample_channel(&source[incoming_x], channel);
        }
    }
}

/** Lazily soften one horizontal light row used by a large structure. */
static void lighting_blur_structure_row(int y) {
    HARD_ASSERT(y >= 0 && y < lighting_height);

    if (structure_rows_valid[y]) {
        return;
    }

    lighting_sample *destination = structure_samples + (size_t)y * (size_t)lighting_width;
    lighting_blur_logical_row(y, structure_blur_row);
    lighting_blur_contiguous_row(structure_blur_row, destination);

    structure_rows_valid[y] = 1;
}

/** Sample the structural field through a vertical triangular filter. */
static lighting_sample lighting_structure_illumination(int x, int y) {
    size_t index = (size_t)y * (size_t)lighting_width + (size_t)x;
    if (structure_illumination_cache_valid[index]) {
        return structure_illumination_cache[index];
    }

    const int radius = LIGHT_STRUCTURE_BLUR_RADIUS;
    uint32_t total[4] = {0};
    uint32_t weights = 0;

    for (int offset = -radius; offset <= radius; offset++) {
        int sample_y = MAX(0, MIN(lighting_height - 1, y + offset));
        uint32_t weight = (uint32_t)(radius + 1 - abs(offset));
        lighting_blur_structure_row(sample_y);
        const lighting_sample *sample =
            &structure_samples[(size_t)sample_y * (size_t)lighting_width + (size_t)x];
        for (size_t channel = 0; channel < 4; channel++) {
            total[channel] += lighting_sample_channel(sample, channel) * weight;
        }
        weights += weight;
    }

    lighting_sample result = {.present = 1};
    for (size_t channel = 0; channel < 4; channel++) {
        lighting_sample_channel_set(&result,
                                    channel,
                                    (uint16_t)((total[channel] + weights / 2) / weights));
    }
    structure_illumination_cache[index] = result;
    structure_illumination_cache_valid[index] = 1;
    return result;
}

void lighting_render(SDL_Surface *destination) {
    HARD_ASSERT(lighting_width > 0);
    HARD_ASSERT(lighting_height > 0);
    HARD_ASSERT(lighting_active);

    bool samples_updated = lighting_update_needed;
    LIGHTING_BENCHMARK_INCREMENT(lighting_context_current, render_calls);
    if (samples_updated) {
        LIGHTING_BENCHMARK_INCREMENT(lighting_context_current, field_rebuilds);
        size_t dirty_pixels = lighting_dirty_pixels(lighting_context_current);
        uint64_t timing_started = lighting_benchmark_timing_start();
        if (dirty_pixels == light_samples_num) {
            LIGHTING_BENCHMARK_INCREMENT(lighting_context_current, field_full_rebuilds);
            switch (lighting_context_current->full_rebuild_cause) {
                case LIGHTING_FULL_REBUILD_CACHE:
                    LIGHTING_BENCHMARK_INCREMENT(lighting_context_current,
                                                 field_full_rebuild_cache);
                    break;
                case LIGHTING_FULL_REBUILD_ACTIVE:
                    LIGHTING_BENCHMARK_INCREMENT(lighting_context_current,
                                                 field_full_rebuild_active);
                    break;
                case LIGHTING_FULL_REBUILD_BOUNDS:
                    LIGHTING_BENCHMARK_INCREMENT(lighting_context_current,
                                                 field_full_rebuild_bounds);
                    break;
                case LIGHTING_FULL_REBUILD_CONTROL:
                    LIGHTING_BENCHMARK_INCREMENT(lighting_context_current,
                                                 field_full_rebuild_control);
                    break;
                case LIGHTING_FULL_REBUILD_NONE:
                    LIGHTING_BENCHMARK_INCREMENT(lighting_context_current,
                                                 field_full_rebuild_other);
                    break;
            }
            lighting_extrapolate();
        } else {
            LIGHTING_BENCHMARK_INCREMENT(lighting_context_current, field_partial_rebuilds);
            lighting_extrapolate_dirty();
        }
        LIGHTING_BENCHMARK_TIMING_FINISH(lighting_context_current, extrapolation, timing_started);
        lighting_structure_illumination_invalidate_dirty(
            lighting_context_current, dirty_pixels == light_samples_num);
        memset(structure_rows_valid, 0, (size_t)lighting_height * sizeof(*structure_rows_valid));
        lighting_cache_key = lighting_pending_cache_key;
        lighting_cache_valid = true;
        lighting_update_needed = false;
        lighting_context_current->full_rebuild_cause = LIGHTING_FULL_REBUILD_NONE;
    }
    lighting_active = false;
    lighting_benchmark_peaks_update();
    if (destination == NULL) {
        return;
    }
    if (!SDL_LockSurface(destination)) {
        LOG(ERROR, "Could not lock map ground for lighting: %s", SDL_GetError());
        lighting_cache_valid = false;
        lighting_update_needed = true;
        LIGHTING_BENCHMARK_INCREMENT(lighting_context_current, render_failures);
        lighting_benchmark_peaks_update();
        return;
    }

    Uint32 colorkey = 0;
    bool has_colorkey = SDL_GetSurfaceColorKey(destination, &colorkey);
    const SDL_PixelFormatDetails *format = SDL_GetPixelFormatDetails(destination->format);
    if (format == NULL || format->bytes_per_pixel != sizeof(Uint32)) {
        LOG(ERROR, "Cannot light a map ground surface that is not 32-bit RGBA.");
        lighting_cache_valid = false;
        lighting_update_needed = true;
        LIGHTING_BENCHMARK_INCREMENT(lighting_context_current, render_failures);
        SDL_UnlockSurface(destination);
        lighting_benchmark_peaks_update();
        return;
    }
    SDL_Palette *palette = SDL_GetSurfacePalette(destination);
    bool direct_argb = destination->format == SDL_PIXELFORMAT_ARGB8888;
    const lighting_context *context = lighting_context_current;
    uint64_t timing_started = lighting_benchmark_timing_start();
    for (int y = 0; y < lighting_height; y++) {
        Uint32 *pixels = (Uint32 *)((Uint8 *)destination->pixels + y * destination->pitch);
        const lighting_sample *row = lighting_sample_row_at_const(context, y);
        int x = 0;
        while (x < lighting_width) {
            int physical_x = x + context->sample_origin_x;
            if (physical_x >= context->width) {
                physical_x -= context->width;
            }
            int length = MIN(lighting_width - x, context->width - physical_x);
            const lighting_sample *samples = row + physical_x;
            for (int offset = 0; offset < length; offset++) {
                int destination_x = x + offset;
                const lighting_sample *sample = &samples[offset];
                if (has_colorkey && pixels[destination_x] == colorkey) {
                    continue;
                }
                uint8_t red, green, blue, alpha;
                if (direct_argb) {
                    red = pixels[destination_x] >> 16;
                    green = pixels[destination_x] >> 8;
                    blue = pixels[destination_x];
                    alpha = pixels[destination_x] >> 24;
                } else {
                    SDL_GetRGBA(pixels[destination_x], format, palette, &red, &green, &blue, &alpha);
                }
                const uint16_t radiance[3] = {sample->red, sample->green, sample->blue};
                uint16_t illumination[3];
                lighting_tone_map_linear(sample->scalar, radiance, illumination);
                red = lighting_multiply_channel(red, illumination[0]);
                green = lighting_multiply_channel(green, illumination[1]);
                blue = lighting_multiply_channel(blue, illumination[2]);
                if (direct_argb) {
                    pixels[destination_x] =
                        (Uint32)alpha << 24 | (Uint32)red << 16 | (Uint32)green << 8 | blue;
                } else {
                    pixels[destination_x] = SDL_MapRGBA(format, palette, red, green, blue, alpha);
                }
            }
            x += length;
        }
    }
    LIGHTING_BENCHMARK_TIMING_FINISH(lighting_context_current, tone_map_multiply, timing_started);

    SDL_UnlockSurface(destination);
}

bool lighting_scene_begin(int width, int height) {
    HARD_ASSERT(width > 0);
    HARD_ASSERT(height > 0);

    lighting_scene_cancel();
    lighting_scene_visibility_clear_invalidated();
    if ((size_t)width > SIZE_MAX / (size_t)height) {
        return false;
    }

    size_t pixels = (size_t)width * (size_t)height;
    if (pixels > SIZE_MAX / sizeof(*lighting_scene_sample_y)) {
        return false;
    }
    if (pixels > SIZE_MAX / sizeof(*lighting_scene_pixels) || pixels > UINT32_MAX) {
        return false;
    }
    lighting_scene_depth = calloc(pixels, sizeof(*lighting_scene_depth));
    lighting_scene_sample_y = malloc(pixels * sizeof(*lighting_scene_sample_y));
    lighting_scene_marked = calloc(pixels, sizeof(*lighting_scene_marked));
    lighting_scene_pixels = malloc(pixels * sizeof(*lighting_scene_pixels));
    if (lighting_scene_depth == NULL || lighting_scene_sample_y == NULL ||
        lighting_scene_marked == NULL || lighting_scene_pixels == NULL) {
        lighting_scene_cancel();
        return false;
    }
    for (size_t i = 0; i < pixels; i++) {
        lighting_scene_sample_y[i] = LIGHTING_SCENE_SAMPLE_Y_RAW;
    }
    lighting_scene_pixels_num = 0;
    lighting_scene_pixels_capacity = pixels;
    lighting_scene_width = width;
    lighting_scene_height = height;
    return true;
}

static void lighting_scene_mark_pixel(size_t index, int8_t owner, int16_t sample_y) {
    if (!lighting_scene_marked[index]) {
        HARD_ASSERT(lighting_scene_pixels_num < lighting_scene_pixels_capacity);
        lighting_scene_marked[index] = 1;
        lighting_scene_pixels[lighting_scene_pixels_num++] = (uint32_t)index;
    }
    lighting_scene_depth[index] = owner;
    lighting_scene_sample_y[index] = sample_y;
}

void lighting_scene_mark_surface(SDL_Surface *source,
                                 int x,
                                 int y,
                                 SDL_Rect *srcrect,
                                 int depth,
                                 int sample_y) {
    if (lighting_scene_depth == NULL || source == NULL) {
        return;
    }

    SDL_Rect source_rect = {
        .x = srcrect != NULL ? srcrect->x : 0,
        .y = srcrect != NULL ? srcrect->y : 0,
        .w = srcrect != NULL ? srcrect->w : source->w,
        .h = srcrect != NULL ? srcrect->h : source->h,
    };
    if (source_rect.w <= 0 || source_rect.h <= 0 || source_rect.x < 0 || source_rect.y < 0 ||
        source_rect.x > source->w - source_rect.w || source_rect.y > source->h - source_rect.h) {
        return;
    }

    bool locked = false;
    if (SDL_MUSTLOCK(source)) {
        if (!SDL_LockSurface(source)) {
            return;
        }
        locked = true;
    }

    Uint32 colorkey = 0;
    bool has_colorkey = SDL_GetSurfaceColorKey(source, &colorkey);
    Uint8 surface_alpha = SDL_ALPHA_OPAQUE;
    SDL_GetSurfaceAlphaMod(source, &surface_alpha);
    const SDL_PixelFormatDetails *format = SDL_GetPixelFormatDetails(source->format);
    SDL_Palette *palette = SDL_GetSurfacePalette(source);
    if (format == NULL) {
        if (locked) {
            SDL_UnlockSurface(source);
        }
        return;
    }
    int8_t owner = (int8_t)MAX(-MAP2_MAX_DEPTH, MIN(MAP2_MAX_DEPTH, depth));
    int16_t owner_sample_y = LIGHTING_SCENE_SAMPLE_Y_RAW;
    if (sample_y != LIGHTING_SCENE_SAMPLE_Y_RAW) {
        owner_sample_y = (int16_t)MAX(INT16_MIN + 1, MIN(INT16_MAX, sample_y));
    }

    /* Most map geometry is fully opaque once its color key has been applied.
     * Preserve the same painter ownership semantics without re-reading every
     * pixel; alpha-bearing sprites use the precise path below. */
    if (!has_colorkey && surface_alpha == SDL_ALPHA_OPAQUE && format != NULL &&
        format->Amask == 0) {
        for (int source_y = 0; source_y < source_rect.h; source_y++) {
            int destination_y = y + source_y;
            if (destination_y < 0 || destination_y >= lighting_scene_height) {
                continue;
            }
            int source_x = MAX(0, -x);
            int destination_x = MAX(0, x);
            int width = MIN(source_rect.w - source_x, lighting_scene_width - destination_x);
            if (width > 0) {
                for (int offset = 0; offset < width; offset++) {
                    lighting_scene_mark_pixel(
                        (size_t)destination_y * (size_t)lighting_scene_width +
                            (size_t)destination_x + (size_t)offset,
                        owner,
                        owner_sample_y);
                }
            }
        }
        if (locked) {
            SDL_UnlockSurface(source);
        }
        return;
    }

    if (surface_alpha == SDL_ALPHA_OPAQUE) {
        lighting_scene_visibility *visibility = lighting_scene_visibility_get(source);
        if (visibility != NULL) {
            for (int source_y = 0; source_y < source_rect.h; source_y++) {
                int destination_y = y + source_y;
                if (destination_y < 0 || destination_y >= lighting_scene_height) {
                    continue;
                }
                int source_x = MAX(0, -x);
                int destination_x = MAX(0, x);
                int width = MIN(source_rect.w - source_x, lighting_scene_width - destination_x);
                int end = source_x + MAX(0, width);
                while (source_x < end) {
                    while (source_x < end &&
                           visibility->pixels[(size_t)(source_rect.y + source_y) *
                                                  (size_t)source->w +
                                              (size_t)(source_rect.x + source_x)] == 0) {
                        source_x++;
                        destination_x++;
                    }
                    int run_start = source_x;
                    int run_destination = destination_x;
                    while (source_x < end &&
                           visibility->pixels[(size_t)(source_rect.y + source_y) *
                                                  (size_t)source->w +
                                              (size_t)(source_rect.x + source_x)] != 0) {
                        source_x++;
                    }
                    int run_width = source_x - run_start;
                    if (run_width > 0) {
                        destination_x += run_width;
                        size_t destination_index = (size_t)destination_y *
                                                        (size_t)lighting_scene_width +
                                                    (size_t)run_destination;
                        for (int offset = 0; offset < run_width; offset++) {
                            lighting_scene_mark_pixel(destination_index + (size_t)offset,
                                                      owner,
                                                      owner_sample_y);
                        }
                    }
                }
            }
            if (locked) {
                SDL_UnlockSurface(source);
            }
            return;
        }
    }

    for (int source_y = 0; source_y < source_rect.h; source_y++) {
        int destination_y = y + source_y;
        if (destination_y < 0 || destination_y >= lighting_scene_height) {
            continue;
        }
        for (int source_x = 0; source_x < source_rect.w; source_x++) {
            int destination_x = x + source_x;
            if (destination_x < 0 || destination_x >= lighting_scene_width) {
                continue;
            }

            Uint32 pixel;
            if (format->bytes_per_pixel == sizeof(Uint32)) {
                const Uint32 *source_pixels = (const Uint32 *)((const Uint8 *)source->pixels +
                                                               (source_rect.y + source_y) *
                                                                   source->pitch);
                pixel = source_pixels[source_rect.x + source_x];
            } else {
                pixel = getpixel(source, source_rect.x + source_x, source_rect.y + source_y);
            }
            if (has_colorkey && pixel == colorkey) {
                continue;
            }
            uint8_t alpha = SDL_ALPHA_OPAQUE;
            if (format->bytes_per_pixel == sizeof(Uint32) && format->Amask != 0) {
                alpha = (uint8_t)((pixel & format->Amask) >> format->Ashift);
            } else if (palette != NULL) {
                uint8_t red, green, blue;
                SDL_GetRGBA(pixel, format, palette, &red, &green, &blue, &alpha);
            }
            if (surface_alpha != SDL_ALPHA_OPAQUE) {
                alpha = (uint8_t)((unsigned int)alpha * surface_alpha / SDL_ALPHA_OPAQUE);
            }
            if (alpha == SDL_ALPHA_TRANSPARENT) {
                continue;
            }
            lighting_scene_mark_pixel((size_t)destination_y * (size_t)lighting_scene_width +
                                          (size_t)destination_x,
                                      owner,
                                      owner_sample_y);
        }
    }

    if (locked) {
        SDL_UnlockSurface(source);
    }
}

/* Compose only pixels recorded by the painter. The list is bounded by the
 * destination surface and makes animation-only work proportional to changed
 * scene geometry instead of the complete viewport. */
__attribute__((optimize("O2"))) void lighting_scene_render(SDL_Surface *destination) {
    if (lighting_scene_depth == NULL || destination == NULL) {
        lighting_scene_cancel();
        return;
    }
    if (destination->w != lighting_scene_width || destination->h != lighting_scene_height) {
        lighting_scene_cancel();
        return;
    }
    if (!SDL_LockSurface(destination)) {
        LOG(ERROR, "Could not lock the complete scene for lighting: %s", SDL_GetError());
        lighting_scene_cancel();
        return;
    }

    Uint32 colorkey = 0;
    bool has_colorkey = SDL_GetSurfaceColorKey(destination, &colorkey);
    const SDL_PixelFormatDetails *format = SDL_GetPixelFormatDetails(destination->format);
    if (format == NULL || format->bytes_per_pixel != sizeof(Uint32)) {
        LOG(ERROR, "Cannot compose a scene surface that is not 32-bit RGBA.");
        SDL_UnlockSurface(destination);
        lighting_scene_cancel();
        return;
    }
    SDL_Palette *palette = SDL_GetSurfacePalette(destination);
    bool direct_argb = destination->format == SDL_PIXELFORMAT_ARGB8888;
    lighting_context *benchmark_context = &lighting_contexts[MAP2_DEPTH_INDEX(0)];
    uint64_t timing_started = lighting_benchmark_timing_start();
    LIGHTING_BENCHMARK_INCREMENT(benchmark_context, whole_field_compositions);

    for (size_t scene_pixel = 0; scene_pixel < lighting_scene_pixels_num; scene_pixel++) {
        LIGHTING_BENCHMARK_INCREMENT(benchmark_context, whole_field_processed_pixels);
        size_t index = lighting_scene_pixels[scene_pixel];
        int y = (int)(index / (size_t)lighting_scene_width);
        int x = (int)(index % (size_t)lighting_scene_width);
        Uint32 *pixels = (Uint32 *)((Uint8 *)destination->pixels + y * destination->pitch);
        if (has_colorkey && pixels[x] == colorkey) {
            continue;
        }

        uint8_t red, green, blue, alpha;
        if (direct_argb) {
            red = pixels[x] >> 16;
            green = pixels[x] >> 8;
            blue = pixels[x];
            alpha = pixels[x] >> 24;
        } else {
            SDL_GetRGBA(pixels[x], format, palette, &red, &green, &blue, &alpha);
        }
        if (alpha == SDL_ALPHA_TRANSPARENT) {
            continue;
        }

        int depth = lighting_scene_depth[(size_t)y * (size_t)lighting_scene_width +
                                         (size_t)x];
        if (depth < -MAP2_MAX_DEPTH || depth > MAP2_MAX_DEPTH ||
            !lighting_contexts[MAP2_DEPTH_INDEX(depth)].cache_valid) {
            depth = 0;
        }
        const lighting_context *context = &lighting_contexts[MAP2_DEPTH_INDEX(depth)];
        int sample_y = lighting_scene_sample_y[(size_t)y * (size_t)lighting_scene_width +
                                               (size_t)x];
        if (sample_y == LIGHTING_SCENE_SAMPLE_Y_RAW) {
            sample_y = y;
        }
        sample_y = MAX(0, MIN(context->height - 1, sample_y));
        const lighting_sample *sample = lighting_sample_at_const(context, x, sample_y);
        const uint16_t radiance[3] = {sample->red, sample->green, sample->blue};
        uint16_t illumination[3];
        if (sample->scalar == 0) {
            illumination[0] = illumination[1] = illumination[2] = 0;
        } else {
            uint32_t neutral = lighting_scene_neutral_linear(sample->scalar);
            for (size_t channel = 0; channel < 3; channel++) {
                uint64_t scaled = (uint64_t)radiance[channel] * neutral;
                scaled = (scaled + sample->scalar / 2) / sample->scalar;
                illumination[channel] =
                    (uint16_t)(scaled > UINT16_MAX ? UINT16_MAX : scaled);
            }
        }
        red = lighting_scene_multiply(red, illumination[0]);
        green = lighting_scene_multiply(green, illumination[1]);
        blue = lighting_scene_multiply(blue, illumination[2]);
        if (direct_argb) {
            pixels[x] = (Uint32)alpha << 24 | (Uint32)red << 16 | (Uint32)green << 8 | blue;
        } else {
            pixels[x] = SDL_MapRGBA(format, palette, red, green, blue, alpha);
        }
        LIGHTING_BENCHMARK_INCREMENT(benchmark_context, whole_field_pixels);
    }
    SDL_UnlockSurface(destination);
    LIGHTING_BENCHMARK_TIMING_FINISH(benchmark_context, tone_map_multiply, timing_started);
    lighting_scene_cancel();
}

void lighting_scene_cancel(void) {
    free(lighting_scene_depth);
    free(lighting_scene_sample_y);
    free(lighting_scene_marked);
    free(lighting_scene_pixels);
    lighting_scene_depth = NULL;
    lighting_scene_sample_y = NULL;
    lighting_scene_marked = NULL;
    lighting_scene_pixels = NULL;
    lighting_scene_pixels_num = 0;
    lighting_scene_pixels_capacity = 0;
    lighting_scene_width = 0;
    lighting_scene_height = 0;
}

/** Ensure the reusable smoothly lit sprite surface is large enough. */
static bool lighting_lit_surface_create(int width, int height) {
    if (lighting_lit_surface != NULL && lighting_lit_surface->w >= width &&
        lighting_lit_surface->h >= height) {
        return true;
    }

    if (lighting_lit_surface != NULL) {
        SDL_DestroySurface(lighting_lit_surface);
        lighting_lit_surface = NULL;
    }

    lighting_lit_surface = lighting_rgba_surface_create(width, height);
    if (lighting_lit_surface == NULL) {
        LOG(ERROR, "Could not create smoothly lit sprite surface: %s", SDL_GetError());
        return false;
    }

    structure_column_bottom =
        xreallocarray(structure_column_bottom, (size_t)width, sizeof(*structure_column_bottom));
    structure_column_illumination = xreallocarray(structure_column_illumination,
                                                  (size_t)width,
                                                  sizeof(*structure_column_illumination));
    surface_set_alpha(lighting_lit_surface, SDL_ALPHA_OPAQUE);
    return true;
}

/** Get a source pixel's intrinsic alpha, excluding whole-surface opacity. */
static uint8_t
lighting_source_alpha(SDL_Surface *source, Uint32 pixel, bool has_colorkey, Uint32 colorkey) {
    if (has_colorkey && pixel == colorkey) {
        return SDL_ALPHA_TRANSPARENT;
    }

    uint8_t red, green, blue, alpha;
    SDL_GetRGBA(pixel,
                SDL_GetPixelFormatDetails(source->format),
                SDL_GetSurfacePalette(source),
                &red,
                &green,
                &blue,
                &alpha);
    return alpha;
}

/** Copy the active portion of the reusable lit-sprite surface. */
static SDL_Surface *lighting_lit_surface_copy(int width, int height) {
    SDL_Surface *copy = SDL_CreateSurface(width, height, lighting_lit_surface->format);
    if (copy == NULL) {
        return NULL;
    }

    if (!SDL_LockSurface(lighting_lit_surface)) {
        SDL_DestroySurface(copy);
        return NULL;
    }
    if (!SDL_LockSurface(copy)) {
        SDL_UnlockSurface(lighting_lit_surface);
        SDL_DestroySurface(copy);
        return NULL;
    }

    size_t row_bytes = (size_t)width * SDL_GetPixelFormatDetails(copy->format)->bytes_per_pixel;
    for (int row = 0; row < height; row++) {
        memcpy((Uint8 *)copy->pixels + row * copy->pitch,
               (Uint8 *)lighting_lit_surface->pixels + row * lighting_lit_surface->pitch,
               row_bytes);
    }

    SDL_UnlockSurface(copy);
    SDL_UnlockSurface(lighting_lit_surface);
    surface_set_alpha(copy, SDL_ALPHA_OPAQUE);
    return copy;
}

static uint64_t lighting_signature_byte(uint64_t signature, uint8_t value) {
    signature ^= value;
    signature *= UINT64_C(1099511628211);
    return signature;
}

static uint64_t lighting_signature_uint32(uint64_t signature, uint32_t value) {
    for (size_t i = 0; i < sizeof(value); i++) {
        signature = lighting_signature_byte(signature, (uint8_t)value);
        value >>= 8;
    }
    return signature;
}

static uint64_t lighting_signature_sample(uint64_t signature, const lighting_sample *sample) {
    uint64_t sample_value = sample->scalar;
    sample_value = (sample_value << 16) | sample->red;
    sample_value = (sample_value << 16) | sample->green;
    sample_value = (sample_value << 16) | sample->blue;
    signature ^= sample_value;
    signature *= UINT64_C(1099511628211);
    signature ^= sample->present;
    return signature * UINT64_C(1099511628211);
}

/**
 * Identify a projected sprite by the radiance it consumes, not by its current
 * viewport position.  The resulting key is stable when the same world sprite
 * is reached again after scrolling, while still preventing a stale lit image
 * from being reused when any covered sample changes.
 */
static uint64_t lighting_projected_signature(int x, int y, const SDL_Rect *source_rect) {
    uint64_t signature = UINT64_C(14695981039346656037);
    signature = lighting_signature_uint32(signature, (uint32_t)source_rect->w);
    signature = lighting_signature_uint32(signature, (uint32_t)source_rect->h);
    /* Include every covered sample so a cache hit is valid only when the
     * complete lit output is equivalent; the fixed work is bounded by the
     * already bounded source surface dimensions. */
    for (int source_y = 0; source_y < source_rect->h; source_y++) {
        int light_y = MAX(0, MIN(lighting_height - 1, y + source_y));
        for (int source_x = 0; source_x < source_rect->w; source_x++) {
            int light_x = MAX(0, MIN(lighting_width - 1, x + source_x));
            const lighting_sample *sample =
                lighting_sample_at_const(lighting_context_current, light_x, light_y);
            signature = lighting_signature_sample(signature, sample);
        }
    }
    return signature;
}

/**
 * Derive the complete per-column illumination that determines one structural
 * sprite's lit output. The source pixels remain identified by the retained
 * source surface and rectangle; the sampled values deliberately exclude
 * viewport position and field generation.
 */
static bool lighting_structure_identity(SDL_Surface *source,
                                        const SDL_Rect *source_rect,
                                        int x,
                                        int sample_y,
                                        bool has_colorkey,
                                        Uint32 colorkey,
                                        const int *retained_column_bottom,
                                        uint64_t *signature) {
    int max_bottom = -1;
    if (retained_column_bottom != NULL) {
        memcpy(structure_column_bottom,
               retained_column_bottom,
               (size_t)source_rect->w * sizeof(*structure_column_bottom));
        for (int source_x = 0; source_x < source_rect->w; source_x++) {
            max_bottom = MAX(max_bottom, structure_column_bottom[source_x]);
        }
    } else {
        if (!SDL_LockSurface(source)) {
            LOG(ERROR, "Could not lock smoothly lit sprite identity: %s", SDL_GetError());
            return false;
        }
        for (int source_x = 0; source_x < source_rect->w; source_x++) {
            int bottom = -1;
            for (int source_y = source_rect->h - 1; source_y >= 0; source_y--) {
                Uint32 source_pixel =
                    getpixel(source, source_rect->x + source_x, source_rect->y + source_y);
                if (lighting_source_alpha(source, source_pixel, has_colorkey, colorkey) !=
                    SDL_ALPHA_TRANSPARENT) {
                    bottom = source_y;
                    break;
                }
            }
            structure_column_bottom[source_x] = bottom;
            max_bottom = MAX(max_bottom, bottom);
        }
        SDL_UnlockSurface(source);
    }

    uint64_t result = UINT64_C(14695981039346656037);
    result = lighting_signature_uint32(result, (uint32_t)source_rect->w);
    for (int source_x = 0; source_x < source_rect->w; source_x++) {
        int bottom = structure_column_bottom[source_x];
        lighting_sample illumination = {2048, 2048, 2048, 2048, 1, 0};
        if (bottom >= 0) {
            int light_x = MAX(0, MIN(lighting_width - 1, x + source_x));
            int light_y = MAX(0, MIN(lighting_height - 1, sample_y - max_bottom + bottom));
            size_t index = (size_t)light_y * (size_t)lighting_width + (size_t)light_x;
            if (structure_illumination_cache_valid[index]) {
                illumination = structure_illumination_cache[index];
            } else {
                illumination = lighting_structure_illumination(light_x, light_y);
            }
        }
        structure_column_illumination[source_x] = illumination;
        result = lighting_signature_sample(result, &illumination);
    }
    *signature = result;
    return true;
}

/** Reuse immutable source-column geometry retained by any equivalent source. */
static const int *lighting_structure_geometry_find(const lighting_context *context,
                                                   SDL_Surface *source,
                                                   const SDL_Rect *source_rect) {
    lighting_sprite_cache_entry *entry, *next;
    HASH_ITER(hh, context->sprite_cache, entry, next) {
        if (entry->key.mode == LIGHTING_SURFACE_STRUCTURE && entry->key.source == source &&
            entry->key.source_x == source_rect->x && entry->key.source_y == source_rect->y &&
            entry->key.source_w == source_rect->w && entry->key.source_h == source_rect->h &&
            entry->structure_column_bottom != NULL) {
            return entry->structure_column_bottom;
        }
    }
    return NULL;
}

static bool lighting_sprite_illumination_count(const SDL_Rect *source_rect,
                                               lighting_surface_mode_t mode,
                                               size_t *count) {
    size_t width = (size_t)source_rect->w;
    size_t height = mode == LIGHTING_SURFACE_STRUCTURE ? 1U : (size_t)source_rect->h;
    if (height != 0 && width > SIZE_MAX / height) {
        return false;
    }
    *count = width * height;
    return *count <= SIZE_MAX / sizeof(lighting_sample);
}

static bool lighting_samples_equal(const lighting_sample *first,
                                   const lighting_sample *second,
                                   size_t count) {
    return memcmp(first, second, count * sizeof(*first)) == 0;
}

/** Compare the full sampled illumination so a hash collision can only miss. */
static bool lighting_sprite_cache_equivalent(const lighting_sprite_cache_entry *entry,
                                             int x,
                                             int y,
                                             const SDL_Rect *source_rect,
                                             lighting_surface_mode_t mode) {
    size_t expected;
    if (!lighting_sprite_illumination_count(source_rect, mode, &expected) ||
        entry->illumination_count != expected) {
        return false;
    }

    if (mode == LIGHTING_SURFACE_STRUCTURE) {
        return lighting_samples_equal(
            entry->illumination, structure_column_illumination, expected);
    }

    for (int source_y = 0; source_y < source_rect->h; source_y++) {
        int light_y = MAX(0, MIN(lighting_height - 1, y + source_y));
        int light_x = MAX(0, MIN(lighting_width - 1, x));
        for (int source_x = 0; source_x < source_rect->w; source_x++) {
            light_x = MAX(0, MIN(lighting_width - 1, x + source_x));
            const lighting_sample *sample =
                lighting_sample_at_const(lighting_context_current, light_x, light_y);
            size_t index = (size_t)source_y * (size_t)source_rect->w + (size_t)source_x;
            if (!lighting_samples_equal(&entry->illumination[index], sample, 1U)) {
                return false;
            }
        }
    }
    return true;
}

static lighting_sample *lighting_sprite_illumination_copy(int x,
                                                          int y,
                                                          const SDL_Rect *source_rect,
                                                          lighting_surface_mode_t mode,
                                                          size_t *count) {
    if (!lighting_sprite_illumination_count(source_rect, mode, count)) {
        return NULL;
    }
    lighting_sample *copy = malloc(*count * sizeof(*copy));
    if (copy == NULL) {
        return NULL;
    }
    if (mode == LIGHTING_SURFACE_STRUCTURE) {
        memcpy(copy, structure_column_illumination, *count * sizeof(*copy));
        return copy;
    }

    for (int source_y = 0; source_y < source_rect->h; source_y++) {
        int light_y = MAX(0, MIN(lighting_height - 1, y + source_y));
        for (int source_x = 0; source_x < source_rect->w; source_x++) {
            int light_x = MAX(0, MIN(lighting_width - 1, x + source_x));
            size_t index = (size_t)source_y * (size_t)source_rect->w + (size_t)source_x;
            copy[index] = *lighting_sample_at_const(lighting_context_current, light_x, light_y);
        }
    }
    return copy;
}

/** Preserve pixel readability when a failed lit transform falls back to an RLE source. */
static void lighting_show_surface_fallback(SDL_Surface *destination,
                                           int x,
                                           int y,
                                           SDL_Rect *source_rect,
                                           SDL_Surface *source) {
    SDL_SetSurfaceRLE(source, false);
    surface_show(destination, x, y, source_rect, source);
}

/** Draw a sprite through the cached continuous light field. */
void lighting_show_surface(SDL_Surface *destination,
                           int x,
                           int y,
                           SDL_Rect *srcrect,
                           SDL_Surface *source,
                           int sample_y,
                           lighting_surface_mode_t mode) {
    HARD_ASSERT(destination != NULL);
    HARD_ASSERT(source != NULL);
    LIGHTING_BENCHMARK_INCREMENT(lighting_context_current, lit_sprite_draws);

    if (!lighting_cache_valid) {
        LIGHTING_BENCHMARK_INCREMENT(lighting_context_current, lit_sprite_fallbacks);
        lighting_show_surface_fallback(destination, x, y, srcrect, source);
        return;
    }

    SDL_Rect source_rect = {
        .x = srcrect != NULL ? srcrect->x : 0,
        .y = srcrect != NULL ? srcrect->y : 0,
        .w = srcrect != NULL ? srcrect->w : source->w,
        .h = srcrect != NULL ? srcrect->h : source->h,
    };
    if (source_rect.w <= 0 || source_rect.h <= 0) {
        LIGHTING_BENCHMARK_INCREMENT(lighting_context_current, lit_sprite_fallbacks);
        lighting_show_surface_fallback(destination, x, y, srcrect, source);
        return;
    }

    uint64_t lookup_started = lighting_benchmark_timing_start();
    Uint32 colorkey = 0;
    bool has_colorkey = SDL_GetSurfaceColorKey(source, &colorkey);
    Uint8 surface_alpha = SDL_ALPHA_OPAQUE;
    SDL_GetSurfaceAlphaMod(source, &surface_alpha);
    bool has_surface_alpha = surface_alpha != SDL_ALPHA_OPAQUE;
    /* Both modes are keyed by their complete output-driving illumination. The
     * retained samples below are compared exactly before a hit is accepted, so
     * the signature is only an index and a collision cannot reuse pixels. */
    uint64_t illumination_signature;
    if (mode == LIGHTING_SURFACE_PROJECTED) {
        illumination_signature = lighting_projected_signature(x, y, &source_rect);
    } else {
        const int *retained_column_bottom =
            lighting_structure_geometry_find(lighting_context_current, source, &source_rect);
        if (!lighting_lit_surface_create(source_rect.w, source_rect.h) ||
            !lighting_structure_identity(source,
                                         &source_rect,
                                         x,
                                         sample_y,
                                         has_colorkey,
                                         colorkey,
                                         retained_column_bottom,
                                         &illumination_signature)) {
            LIGHTING_BENCHMARK_INCREMENT(lighting_context_current, lit_sprite_fallbacks);
            lighting_show_surface_fallback(destination, x, y, srcrect, source);
            return;
        }
    }

    lighting_sprite_cache_key cache_key;
    memset(&cache_key, 0, sizeof(cache_key));
    cache_key.source = source;
    cache_key.source_x = source_rect.x;
    cache_key.source_y = source_rect.y;
    cache_key.source_w = source_rect.w;
    cache_key.source_h = source_rect.h;
    cache_key.illumination_signature = illumination_signature;
    cache_key.mode = mode;
    cache_key.surface_alpha = surface_alpha;
    lighting_sprite_cache_entry *cached;
    HASH_FIND(hh, lighting_context_current->sprite_cache, &cache_key, sizeof(cache_key), cached);
    if (cached != NULL && !lighting_sprite_cache_equivalent(cached, x, y, &source_rect, mode)) {
        cached = NULL;
    }
    LIGHTING_BENCHMARK_TIMING_FINISH(lighting_context_current, sprite_lookup, lookup_started);
    LIGHTING_BENCHMARK_INCREMENT(lighting_context_current, lit_sprite_lookups);
    LIGHTING_BENCHMARK_MODE_INCREMENT(lighting_context_current, mode, lookups);
    if (cached != NULL) {
        LIGHTING_BENCHMARK_INCREMENT(lighting_context_current, lit_sprite_hits);
        LIGHTING_BENCHMARK_MODE_INCREMENT(lighting_context_current, mode, hits);
        lighting_sprite_cache_touch(lighting_context_current, cached);
        surface_show(destination, x, y, NULL, cached->surface);
        return;
    }
    LIGHTING_BENCHMARK_INCREMENT(lighting_context_current, lit_sprite_misses);
    LIGHTING_BENCHMARK_MODE_INCREMENT(lighting_context_current, mode, misses);
    LIGHTING_BENCHMARK_MODE_INCREMENT(lighting_context_current, mode, constructions);
    uint64_t construction_started = lighting_benchmark_timing_start();

    bool create_failed = false;
#ifdef ATRINIK_WIDGET_TESTS
    create_failed = lighting_benchmark_fault_take(LIGHTING_BENCHMARK_FAULT_CREATE);
#endif
    if (create_failed || !lighting_lit_surface_create(source_rect.w, source_rect.h)) {
        LIGHTING_BENCHMARK_TIMING_FINISH(lighting_context_current,
                                         sprite_construction,
                                         construction_started);
        LIGHTING_BENCHMARK_INCREMENT(lighting_context_current, lit_sprite_fallbacks);
        lighting_show_surface_fallback(destination, x, y, srcrect, source);
        return;
    }

    bool source_locked = false;

    if (mode == LIGHTING_SURFACE_STRUCTURE) {
        bool lock_failed = false;
#ifdef ATRINIK_WIDGET_TESTS
        lock_failed = lighting_benchmark_fault_take(LIGHTING_BENCHMARK_FAULT_STRUCTURE_LOCK);
#endif
        if (lock_failed || !SDL_LockSurface(source)) {
            LOG(ERROR, "Could not lock smoothly lit sprite: %s", SDL_GetError());
            LIGHTING_BENCHMARK_TIMING_FINISH(lighting_context_current,
                                             sprite_construction,
                                             construction_started);
            LIGHTING_BENCHMARK_INCREMENT(lighting_context_current, lit_sprite_fallbacks);
            lighting_show_surface_fallback(destination, x, y, srcrect, source);
            return;
        }
        source_locked = true;
    }

    if (!source_locked) {
        bool lock_failed = false;
#ifdef ATRINIK_WIDGET_TESTS
        lock_failed = lighting_benchmark_fault_take(LIGHTING_BENCHMARK_FAULT_PROJECTED_LOCK);
#endif
        if (lock_failed || !SDL_LockSurface(source)) {
            LOG(ERROR, "Could not lock smoothly lit sprite: %s", SDL_GetError());
            LIGHTING_BENCHMARK_TIMING_FINISH(lighting_context_current,
                                             sprite_construction,
                                             construction_started);
            LIGHTING_BENCHMARK_INCREMENT(lighting_context_current, lit_sprite_fallbacks);
            lighting_show_surface_fallback(destination, x, y, srcrect, source);
            return;
        }
    }
    bool destination_lock_failed = false;
#ifdef ATRINIK_WIDGET_TESTS
    destination_lock_failed =
        lighting_benchmark_fault_take(LIGHTING_BENCHMARK_FAULT_DESTINATION_LOCK);
#endif
    if (destination_lock_failed || !SDL_LockSurface(lighting_lit_surface)) {
        LOG(ERROR, "Could not lock smoothly lit sprite surface: %s", SDL_GetError());
        SDL_UnlockSurface(source);
        LIGHTING_BENCHMARK_TIMING_FINISH(lighting_context_current,
                                         sprite_construction,
                                         construction_started);
        LIGHTING_BENCHMARK_INCREMENT(lighting_context_current, lit_sprite_fallbacks);
        lighting_show_surface_fallback(destination, x, y, srcrect, source);
        return;
    }

    for (int source_y = 0; source_y < source_rect.h; source_y++) {
        Uint32 *destination_pixels = (Uint32 *)((Uint8 *)lighting_lit_surface->pixels +
                                                source_y * lighting_lit_surface->pitch);

        for (int source_x = 0; source_x < source_rect.w; source_x++) {
            Uint32 source_pixel =
                getpixel(source, source_rect.x + source_x, source_rect.y + source_y);
            uint8_t red = 0;
            uint8_t green = 0;
            uint8_t blue = 0;
            uint8_t source_alpha = SDL_ALPHA_OPAQUE;
            if (has_colorkey && source_pixel == colorkey) {
                source_alpha = SDL_ALPHA_TRANSPARENT;
            } else {
                SDL_GetRGBA(source_pixel,
                            SDL_GetPixelFormatDetails(source->format),
                            SDL_GetSurfacePalette(source),
                            &red,
                            &green,
                            &blue,
                            &source_alpha);
            }
            if (has_surface_alpha) {
                source_alpha =
                    (uint8_t)((unsigned int)source_alpha * surface_alpha / SDL_ALPHA_OPAQUE);
            }

            lighting_sample illumination;
            if (mode == LIGHTING_SURFACE_STRUCTURE) {
                illumination = structure_column_illumination[source_x];
            } else {
                int light_x = MAX(0, MIN(lighting_width - 1, x + source_x));
                int light_y = MAX(0, MIN(lighting_height - 1, y + source_y));
                illumination =
                    *lighting_sample_at_const(lighting_context_current, light_x, light_y);
            }
            const uint16_t radiance[3] = {
                illumination.red,
                illumination.green,
                illumination.blue,
            };
            uint16_t linear[3];
            lighting_tone_map_linear(illumination.scalar, radiance, linear);
            red = lighting_multiply_channel(red, linear[0]);
            green = lighting_multiply_channel(green, linear[1]);
            blue = lighting_multiply_channel(blue, linear[2]);
            destination_pixels[source_x] =
                SDL_MapRGBA(SDL_GetPixelFormatDetails(lighting_lit_surface->format),
                            SDL_GetSurfacePalette(lighting_lit_surface),
                            red,
                            green,
                            blue,
                            source_alpha);
        }
    }

    SDL_UnlockSurface(lighting_lit_surface);
    SDL_UnlockSurface(source);

    SDL_Rect lit_rect = {.x = 0, .y = 0, .w = source_rect.w, .h = source_rect.h};
    size_t illumination_count = 0;
    lighting_sample *illumination =
        lighting_sprite_illumination_copy(x, y, &source_rect, mode, &illumination_count);
    size_t illumination_bytes = illumination_count * sizeof(*illumination);
    int *retained_column_bottom = NULL;
    size_t structure_geometry_bytes = 0;
    if (mode == LIGHTING_SURFACE_STRUCTURE &&
        (size_t)source_rect.w <= SIZE_MAX / sizeof(*retained_column_bottom)) {
        structure_geometry_bytes = (size_t)source_rect.w * sizeof(*retained_column_bottom);
        retained_column_bottom = malloc(structure_geometry_bytes);
        if (retained_column_bottom != NULL) {
            memcpy(retained_column_bottom, structure_column_bottom, structure_geometry_bytes);
        }
    }
    size_t cache_bytes =
        lighting_sprite_cache_charge((size_t)lighting_lit_surface->pitch, (size_t)source_rect.h);
    if (illumination == NULL ||
        (mode == LIGHTING_SURFACE_STRUCTURE && retained_column_bottom == NULL) ||
        cache_bytes > SIZE_MAX - illumination_bytes ||
        cache_bytes + illumination_bytes > SIZE_MAX - structure_geometry_bytes) {
        free(illumination);
        free(retained_column_bottom);
        illumination = NULL;
        cache_bytes = SIZE_MAX;
    } else {
        cache_bytes += illumination_bytes + structure_geometry_bytes;
    }
    if (cache_bytes <= LIGHTING_SPRITE_CACHE_MAX_BYTES) {
        SDL_Surface *copy = lighting_lit_surface_copy(source_rect.w, source_rect.h);
        if (copy != NULL) {
            LIGHTING_BENCHMARK_TIMING_FINISH(lighting_context_current,
                                             sprite_construction,
                                             construction_started);
            lighting_sprite_cache_reserve(lighting_context_current, cache_bytes);
            lighting_sprite_cache_entry *entry = xcalloc(1, sizeof(*entry));
            entry->key = cache_key;
            entry->illumination = illumination;
            entry->illumination_count = illumination_count;
            entry->structure_column_bottom = retained_column_bottom;
            entry->surface = copy;
            entry->bytes = cache_bytes;
            HASH_ADD(hh, lighting_context_current->sprite_cache, key, sizeof(entry->key), entry);
            lighting_sprite_cache_append(lighting_context_current, entry);
            lighting_context_current->sprite_cache_bytes += cache_bytes;
            LIGHTING_BENCHMARK_INCREMENT(lighting_context_current, lit_sprite_insertions);
            LIGHTING_BENCHMARK_MODE_INCREMENT(lighting_context_current, mode, insertions);
            lighting_benchmark_peaks_update();
            surface_show(destination, x, y, NULL, copy);
            return;
        }
    }
    free(illumination);
    free(retained_column_bottom);

    LIGHTING_BENCHMARK_INCREMENT(lighting_context_current, lit_sprite_fallbacks);
    LIGHTING_BENCHMARK_TIMING_FINISH(lighting_context_current,
                                     sprite_construction,
                                     construction_started);
    surface_show(destination, x, y, &lit_rect, lighting_lit_surface);
}

#undef light_samples
#undef structure_samples
#undef structure_illumination_cache
#undef structure_blur_row
#undef structure_rows_valid
#undef lighting_dirty_spans
#undef lighting_dirty_span_num
#undef structure_illumination_cache_valid
#undef light_samples_num
#undef lighting_width
#undef lighting_height
#undef lighting_active
#undef lighting_cache_valid
#undef lighting_update_needed
#undef lighting_cache_key
#undef lighting_pending_cache_key

void lighting_deinit(void) {
    lighting_scene_cancel();
    lighting_scene_visibility_clear();
    for (size_t i = 0; i < arraysize(lighting_contexts); i++) {
        lighting_context_free(&lighting_contexts[i]);
    }

    lighting_context_current = &lighting_contexts[MAP2_DEPTH_INDEX(0)];

    if (lighting_lit_surface != NULL) {
        SDL_DestroySurface(lighting_lit_surface);
        lighting_lit_surface = NULL;
    }

    free(structure_column_bottom);
    structure_column_bottom = NULL;
    free(structure_column_illumination);
    structure_column_illumination = NULL;
}

#undef LIGHTING_BENCHMARK_INCREMENT
#undef LIGHTING_BENCHMARK_ADD
#undef LIGHTING_BENCHMARK_TIMING_FINISH
