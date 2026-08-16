/*************************************************************************
 *           Atrinik, a Multiplayer Online Role Playing Game             *
 *                                                                       *
 *   Copyright (C) 2009-2026 Zoey Rose and Atrinik Development Team      *
 *                                                                       *
 * Fork from Crossfire (Multiplayer game for X-windows).                 *
 *                                                                       *
 * This program is free software; you can redistribute it and/or modify  *
 * it under the terms of the GNU General Public License as published by  *
 * the Free Software Foundation; either version 2 of the License, or     *
 * (at your option) any later version.                                   *
 *                                                                       *
 * This program is distributed in the hope that it will be useful,       *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 * GNU General Public License for more details.                          *
 *                                                                       *
 * You should have received a copy of the GNU General Public License     *
 * along with this program; if not, write to the Free Software           *
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.             *
 *                                                                       *
 * The author can be reached at admin@atrinik.org                        *
 ************************************************************************/

/**
 * @file
 * Sprite related functions.
 */

#include <global.h>
#include <image_codec.h>
#include <wrapper.h>
#include <surface_primitives.h>
#include <toolkit/string.h>
#include <toolkit/colorspace.h>

/**
 * Structure used to cache sprite surfaces that have had special effects
 * rendered on them.
 */
typedef struct sprite_cache {
    char *name; ///< Name of the sprite. Used for hash table lookups.
    SDL_Surface *source; ///< Render-owned source this transformed result depends on.
    SDL_Surface *surface; ///< The sprite's surface.
    size_t estimated_bytes; ///< Entry, key, surface, and pixel storage estimate.
    time_t last_used; ///< Last time the sprite was used.
    uint64_t use_sequence; ///< Monotonic tie-breaker for deterministic eviction.
    struct sprite_cache *lru_previous;
    struct sprite_cache *lru_next;
    UT_hash_handle hh; ///< Hash handle.
} sprite_cache_t;

/** Format holder for red_scale(), fow_scale() and grey_scale() functions. */
SDL_Surface *FormatHolder;

/** Darkness alpha values. */
static int dark_alpha[DARK_LEVELS] = {0, 44, 80, 117, 153, 190, 226};

/**
 * The sprite cache hash table.
 */
static sprite_cache_t *sprites_cache = NULL;
static sprite_cache_t *sprite_cache_lru_oldest;
static sprite_cache_t *sprite_cache_lru_newest;
static sprite_cache_statistics_t sprite_cache_statistics;
static size_t sprite_cache_bytes;
static uint64_t sprite_cache_use_sequence;
static bool sprite_cache_clock_overridden;
static time_t sprite_cache_clock_override;

static time_t sprite_cache_now(void) {
    return sprite_cache_clock_overridden ? sprite_cache_clock_override : time(NULL);
}

void sprite_cache_clock_override_set(time_t now) {
    sprite_cache_clock_override = now;
    sprite_cache_clock_overridden = true;
}

void sprite_cache_clock_override_clear(void) {
    sprite_cache_clock_overridden = false;
    sprite_cache_clock_override = 0;
}

static size_t sprite_cache_estimated_bytes(const sprite_cache_t *cache) {
    HARD_ASSERT(cache != NULL);
    HARD_ASSERT(cache->surface != NULL);

    size_t bytes = sizeof(*cache) + strlen(cache->name) + 1 + sizeof(*cache->surface);
    size_t pitch = cache->surface->pitch >= 0 ? (size_t)cache->surface->pitch
                                              : (size_t)(-(int64_t)cache->surface->pitch);
    size_t height = (size_t)cache->surface->h;
    if (height != 0 && pitch > (SIZE_MAX - bytes) / height) {
        return SIZE_MAX;
    }
    return bytes + pitch * height;
}

static void sprite_cache_free(sprite_cache_t *cache);

static void sprite_cache_lru_append(sprite_cache_t *cache) {
    cache->lru_previous = sprite_cache_lru_newest;
    cache->lru_next = NULL;
    if (sprite_cache_lru_newest != NULL) {
        sprite_cache_lru_newest->lru_next = cache;
    } else {
        sprite_cache_lru_oldest = cache;
    }
    sprite_cache_lru_newest = cache;
}

static void sprite_cache_lru_touch(sprite_cache_t *cache) {
    if (sprite_cache_lru_newest == cache) {
        return;
    }
    if (cache->lru_previous != NULL) {
        cache->lru_previous->lru_next = cache->lru_next;
    } else {
        sprite_cache_lru_oldest = cache->lru_next;
    }
    if (cache->lru_next != NULL) {
        cache->lru_next->lru_previous = cache->lru_previous;
    }
    cache->use_sequence = ++sprite_cache_use_sequence;
    sprite_cache_lru_append(cache);
}

void sprite_cache_statistics_reset(void) {
    size_t entries = sprite_cache_statistics.entries;
    size_t estimated_bytes = sprite_cache_statistics.estimated_bytes;
    sprite_cache_statistics = (sprite_cache_statistics_t){
        .entries = entries,
        .estimated_bytes = estimated_bytes,
        .peak_entries = entries,
        .peak_estimated_bytes = estimated_bytes,
    };
}

void sprite_cache_statistics_get(sprite_cache_statistics_t *statistics) {
    HARD_ASSERT(statistics != NULL);
    *statistics = sprite_cache_statistics;
}

/**
 * Initialize the sprite system.
 */
void sprite_init_system(void) {
    FormatHolder = surface_create_rgb(0, 1, 1, 32, 0xFF000000, 0x00FF0000, 0x0000FF00, 0x000000FF);
    HARD_ASSERT(FormatHolder != NULL);
    surface_set_alpha(FormatHolder, SDL_ALPHA_OPAQUE);
}

/**
 * Load sprite file.
 *
 * @param fname
 * Sprite filename.
 * @param flags
 * Flags for the sprite.
 * @return
 * NULL if failed, the sprite otherwise.
 */
sprite_struct *sprite_load_file(char *fname, uint32_t flags) {
    sprite_struct *sprite = sprite_tryload_file(fname, flags, NULL);
    if (sprite == NULL) {
        LOG(ERROR, "Can't load sprite %s", fname);
        return NULL;
    }

    return sprite;
}

/**
 * Try to load a sprite image file.
 *
 * @param fname
 * Sprite filename
 * @param flag
 * Flags
 * @param rwop
 * Pointer to memory for the image
 * @return
 * The sprite if success, NULL otherwise
 */
sprite_struct *sprite_tryload_file(char *fname, uint32_t flag, SDL_IOStream *rwop) {
    SDL_Surface *bitmap;
    if (fname != NULL) {
        bitmap = IMG_Load_wrapper(fname);
        if (bitmap == NULL) {
            return NULL;
        }
    } else {
        bitmap = image_codec_load_png_io(rwop);
    }
    if (bitmap == NULL) {
        return NULL;
    }

    return sprite_from_surface(bitmap, flag, true);
}

sprite_struct *sprite_from_surface(SDL_Surface *bitmap, uint32_t flag, bool enable_rle) {
    if (bitmap == NULL) {
        return NULL;
    }

    sprite_struct *sprite = xcalloc(1, sizeof(*sprite));
    if (sprite == NULL) {
        SDL_DestroySurface(bitmap);
        return NULL;
    }

    SDL_Palette *palette = SDL_GetSurfacePalette(bitmap);

    if (palette == NULL && (flag & SURFACE_FLAG_COLKEY_16M)) {
        /* Force a true color png to colorkey. Default ckey is black (0). */
        SDL_SetSurfaceColorKey(bitmap, true, 0);
    }

    if (!surface_ensure_blittable(&bitmap)) {
        SDL_DestroySurface(bitmap);
        free(sprite);
        return NULL;
    }

    sprite_borders_get(bitmap, sprite);
    if (enable_rle) {
        SDL_SetSurfaceRLE(bitmap, true);
    }
    sprite->bitmap = bitmap;

    if (flag & (SURFACE_FLAG_DISPLAYFORMATALPHA | SURFACE_FLAG_DISPLAYFORMAT)) {
        sprite->bitmap = flag & SURFACE_FLAG_DISPLAYFORMATALPHA ? surface_to_display_alpha(bitmap)
                                                                : surface_to_display(bitmap);
        SDL_DestroySurface(bitmap);
        if (sprite->bitmap == NULL) {
            free(sprite);
            return NULL;
        }
    }

    return sprite;
}

/**
 * Free a sprite.
 *
 * @param sprite
 * Sprite to free.
 */
void sprite_free_sprite(sprite_struct *sprite) {
    if (sprite == NULL) {
        return;
    }

    if (sprite->bitmap != NULL) {
        SDL_DestroySurface(sprite->bitmap);
    }

    free(sprite);
}

/**
 * Find a sprite in the sprite cache.
 *
 * @param name
 * Name of the sprite to find.
 * @return
 * Sprite if found, NULL otherwise.
 */
static sprite_cache_t *sprite_cache_find(const char *name) {
    HARD_ASSERT(name != NULL);

    sprite_cache_statistics.lookups++;
    sprite_cache_t *cache;
    HASH_FIND_STR(sprites_cache, name, cache);

    if (cache != NULL) {
        sprite_cache_statistics.hits++;
        cache->last_used = sprite_cache_now();
        sprite_cache_lru_touch(cache);
    } else {
        sprite_cache_statistics.misses++;
    }

    return cache;
}

/**
 * Create a new sprite cache entry.
 *
 * @param name
 * Name of the cache entry.
 * @return
 * Created sprite entry.
 */
static sprite_cache_t *sprite_cache_create(const char *name) {
    HARD_ASSERT(name != NULL);

    sprite_cache_t *cache = xcalloc(1, sizeof(*cache));
    cache->name = xstrdup(name);
    cache->last_used = sprite_cache_now();
    cache->use_sequence = ++sprite_cache_use_sequence;
    return cache;
}

static void sprite_cache_remove_entry(sprite_cache_t *cache, bool eviction) {
    HARD_ASSERT(cache != NULL);
    HASH_DEL(sprites_cache, cache);
    if (cache->lru_previous != NULL) {
        cache->lru_previous->lru_next = cache->lru_next;
    } else {
        sprite_cache_lru_oldest = cache->lru_next;
    }
    if (cache->lru_next != NULL) {
        cache->lru_next->lru_previous = cache->lru_previous;
    } else {
        sprite_cache_lru_newest = cache->lru_previous;
    }
    HARD_ASSERT(sprite_cache_statistics.entries != 0);
    sprite_cache_statistics.entries--;
    HARD_ASSERT(sprite_cache_bytes >= cache->estimated_bytes);
    sprite_cache_bytes -= cache->estimated_bytes;
    sprite_cache_statistics.estimated_bytes = sprite_cache_bytes;
    if (eviction) {
        sprite_cache_statistics.evictions++;
    }
    sprite_cache_free(cache);
}

static void sprite_cache_reserve(size_t bytes) {
    while (sprites_cache != NULL &&
           (sprite_cache_bytes > SPRITE_CACHE_MAX_BYTES - MIN(bytes, SPRITE_CACHE_MAX_BYTES) ||
            HASH_COUNT(sprites_cache) >= SPRITE_CACHE_MAX_ENTRIES)) {
        sprite_cache_remove_entry(sprite_cache_lru_oldest, true);
    }
}

/**
 * Add a sprite cache entry to the sprite cache.
 *
 * @param cache
 * Cache entry to add.
 */
static bool sprite_cache_add(sprite_cache_t *cache) {
    HARD_ASSERT(cache != NULL);
    HARD_ASSERT(cache->surface != NULL);
    cache->estimated_bytes = sprite_cache_estimated_bytes(cache);
    if (cache->estimated_bytes > SPRITE_CACHE_MAX_BYTES) {
        sprite_cache_statistics.rejections++;
        return false;
    }
    sprite_cache_reserve(cache->estimated_bytes);
    HASH_ADD_KEYPTR(hh, sprites_cache, cache->name, strlen(cache->name), cache);
    sprite_cache_lru_append(cache);
    sprite_cache_statistics.insertions++;
    sprite_cache_statistics.entries++;
    sprite_cache_bytes += cache->estimated_bytes;
    sprite_cache_statistics.estimated_bytes = sprite_cache_bytes;
    sprite_cache_statistics.peak_entries =
        MAX(sprite_cache_statistics.peak_entries, sprite_cache_statistics.entries);
    sprite_cache_statistics.peak_estimated_bytes =
        MAX(sprite_cache_statistics.peak_estimated_bytes, sprite_cache_statistics.estimated_bytes);
    return true;
}

/**
 * Remove a sprite entry from the sprite cache.
 *
 * @param cache
 * Cache entry to remove.
 */
static void sprite_cache_remove(sprite_cache_t *cache) {
    HARD_ASSERT(cache != NULL);
    sprite_cache_remove_entry(cache, false);
}

/**
 * Free the specified sprite cache entry.
 * @param cache
 */
static void sprite_cache_free(sprite_cache_t *cache) {
    HARD_ASSERT(cache != NULL);

    free(cache->name);
    lighting_invalidate_surface(cache->surface);
    SDL_DestroySurface(cache->surface);
    free(cache);
}

/**
 * Free all the sprite cache entries.
 */
void sprite_cache_free_all(void) {
    sprite_cache_t *cache, *tmp;
    HASH_ITER(hh, sprites_cache, cache, tmp) {
        sprite_cache_remove(cache);
    }
}

/** Remove transformed entries derived from one render-owned source. */
static void sprite_cache_invalidate_source(SDL_Surface *source) {
    sprite_cache_t *cache, *next;
    HASH_ITER(hh, sprites_cache, cache, next) {
        if (cache->source == source) {
            sprite_cache_remove(cache);
        }
    }
}

void sprite_invalidate_surface(SDL_Surface *source) {
    if (source == NULL) {
        return;
    }
    sprite_cache_invalidate_source(source);
    lighting_invalidate_surface(source);
}

void sprite_free_rendered(sprite_struct *sprite) {
    if (sprite == NULL) {
        return;
    }

    if (sprite->bitmap != NULL) {
        sprite_invalidate_surface(sprite->bitmap);
    }
    sprite_free_sprite(sprite);
}

/**
 * Free unused sprite cache entries.
 */
static void sprite_cache_gc_run(bool force) {
    sprite_cache_statistics.gc_runs++;
    uint64_t gc_started_ns = SDL_GetTicksNS();
    time_t now = sprite_cache_now();

    struct timeval tv1;
    gettimeofday(&tv1, NULL);

    sprite_cache_t *cache, *tmp;
    HASH_ITER(hh, sprites_cache, cache, tmp) {
        if (now - cache->last_used >= SPRITE_CACHE_GC_FREE_TIME) {
            sprite_cache_remove(cache);
            sprite_cache_statistics.gc_removals++;
        }

        /* Avoid executing this loop for too long. */
        struct timeval tv2;
        if (!force && gettimeofday(&tv2, NULL) == 0 &&
            tv2.tv_usec - tv1.tv_usec >= SPRITE_CACHE_GC_MAX_TIME) {
            break;
        }
    }

    sprite_cache_statistics.gc_time_ns += SDL_GetTicksNS() - gc_started_ns;
}

void sprite_cache_gc(void) {
    sprite_cache_gc_run(false);
}

#ifdef ATRINIK_WIDGET_TESTS
void sprite_cache_gc_force(void) {
    sprite_cache_gc_run(true);
}

bool sprite_benchmark_source_lifetime_complete(void) {
    sprite_cache_t *dependency = NULL;
    sprite_cache_t *cache, *next;
    HASH_ITER(hh, sprites_cache, cache, next) {
        if (cache->source != NULL &&
            lighting_benchmark_source_address_retained((uintptr_t)cache->surface)) {
            dependency = cache;
            break;
        }
    }
    if (dependency == NULL) {
        return false;
    }

    SDL_Surface *source = dependency->source;
    uintptr_t source_address = (uintptr_t)source;
    uintptr_t transformed_address = (uintptr_t)dependency->surface;
    sprite_invalidate_surface(source);

    HASH_ITER(hh, sprites_cache, cache, next) {
        if (cache->source == source) {
            return false;
        }
    }
    return !lighting_benchmark_source_address_retained(source_address) &&
           !lighting_benchmark_source_address_retained(transformed_address);
}
#endif

/**
 * Creates a red version of the specified sprite surface.
 *
 * Used for the infravision effect.
 *
 * @param surface
 * Surface.
 * @return
 * New surface.
 */
static SDL_Surface *sprite_effect_red(SDL_Surface *surface) {
    SDL_Surface *tmp = SDL_ConvertSurface(surface, FormatHolder->format);
    if (tmp == NULL) {
        return NULL;
    }

    for (int y = 0; y < tmp->h; y++) {
        for (int x = 0; x < tmp->w; x++) {
            Uint8 r, g, b, a;
            pixel_format_get_rgba(getpixel(tmp, x, y), tmp->format, &r, &g, &b, &a);
            r = (Uint8)(0.212671 * r + 0.715160 * g + 0.072169 * b);
            g = b = 0;
            putpixel(tmp, x, y, pixel_format_map_rgba(tmp->format, r, g, b, a));
        }
    }

    SDL_Surface *ret = surface_to_display_alpha(tmp);
    SDL_DestroySurface(tmp);
    return ret;
}

/**
 * Creates a gray version of the specified sprite surface.
 *
 * Used for the invisible effect.
 *
 * @param surface
 * Surface.
 * @return
 * New surface.
 */
static SDL_Surface *sprite_effect_gray(SDL_Surface *surface) {
    SDL_Surface *tmp = SDL_ConvertSurface(surface, FormatHolder->format);
    if (tmp == NULL) {
        return NULL;
    }

    for (int y = 0; y < tmp->h; y++) {
        for (int x = 0; x < tmp->w; x++) {
            Uint8 r, g, b, a;
            pixel_format_get_rgba(getpixel(tmp, x, y), tmp->format, &r, &g, &b, &a);
            r = g = b = (Uint8)(0.212671 * r + 0.715160 * g + 0.072169 * b);
            putpixel(tmp, x, y, pixel_format_map_rgba(tmp->format, r, g, b, a));
        }
    }

    SDL_Surface *ret = surface_to_display_alpha(tmp);
    SDL_DestroySurface(tmp);
    return ret;
}

/**
 * Creates somewhat gray version of the specified sprite surface.
 *
 * Used for the fog of war effect.
 *
 * @param surface
 * Surface.
 * @return
 * New surface.
 */
static SDL_Surface *sprite_effect_fow(SDL_Surface *surface) {
    SDL_Surface *tmp = SDL_ConvertSurface(surface, FormatHolder->format);
    if (tmp == NULL) {
        return NULL;
    }

    for (int y = 0; y < tmp->h; y++) {
        for (int x = 0; x < tmp->w; x++) {
            Uint8 r, g, b, a;
            pixel_format_get_rgba(getpixel(tmp, x, y), tmp->format, &r, &g, &b, &a);
            r = (Uint8)((0.212671 * r + 0.715160 * g + 0.072169 * b) * 0.34);
            g = b = r;
            b += 16;
            putpixel(tmp, x, y, pixel_format_map_rgba(tmp->format, r, g, b, a));
        }
    }

    SDL_Surface *ret = surface_to_display_alpha(tmp);
    SDL_DestroySurface(tmp);
    return ret;
}

/** Create an outline-only version of a sprite without exposing pixels behind it. */
SDL_Surface *sprite_outline_create(SDL_Surface *surface, const SDL_Color *color) {
    SDL_Surface *outline = SDL_CreateSurface(surface->w + SPRITE_GLOW_SIZE * 2,
                                             surface->h + SPRITE_GLOW_SIZE * 2,
                                             FormatHolder->format);
    if (outline == NULL) {
        return NULL;
    }

    Uint32 transparent = pixel_format_map_rgba(outline->format, 0, 0, 0, 0);
    SDL_FillSurfaceRect(outline, NULL, transparent);

    bool source_locked = false;
    bool outline_locked = false;
    if (SDL_MUSTLOCK(surface)) {
        if (!SDL_LockSurface(surface)) {
            SDL_DestroySurface(outline);
            return NULL;
        }
        source_locked = true;
    }
    if (SDL_MUSTLOCK(outline)) {
        if (!SDL_LockSurface(outline)) {
            if (source_locked) {
                SDL_UnlockSurface(surface);
            }
            SDL_DestroySurface(outline);
            return NULL;
        }
        outline_locked = true;
    }

    Uint32 edge = pixel_format_map_rgba(outline->format, color->r, color->g, color->b, 235);
    Uint32 halo = pixel_format_map_rgba(outline->format, color->r, color->g, color->b, 90);
    for (int radius = SPRITE_GLOW_SIZE; radius >= 1; radius--) {
        Uint32 pixel = radius == 1 ? edge : halo;
        for (int source_y = 0; source_y < surface->h; source_y++) {
            for (int source_x = 0; source_x < surface->w; source_x++) {
                if (!surface_pixel_visible(surface, source_x, source_y)) {
                    continue;
                }

                for (int offset_y = -radius; offset_y <= radius; offset_y++) {
                    for (int offset_x = -radius; offset_x <= radius; offset_x++) {
                        if (abs(offset_x) != radius && abs(offset_y) != radius) {
                            continue;
                        }

                        putpixel(outline,
                                 source_x + SPRITE_GLOW_SIZE + offset_x,
                                 source_y + SPRITE_GLOW_SIZE + offset_y,
                                 pixel);
                    }
                }
            }
        }
    }

    /* The dilation passes also touch visible source locations near other
     * silhouette pixels. Keep the overlay outline-only by clearing them last. */
    for (int source_y = 0; source_y < surface->h; source_y++) {
        for (int source_x = 0; source_x < surface->w; source_x++) {
            if (surface_pixel_visible(surface, source_x, source_y)) {
                putpixel(outline,
                         source_x + SPRITE_GLOW_SIZE,
                         source_y + SPRITE_GLOW_SIZE,
                         transparent);
            }
        }
    }

    if (outline_locked) {
        SDL_UnlockSurface(outline);
    }
    if (source_locked) {
        SDL_UnlockSurface(surface);
    }
    surface_set_alpha(outline, SDL_ALPHA_OPAQUE);
    return outline;
}

/**
 * Creates a glow effect for the specified sprite surface.
 *
 * @param surface
 * Surface.
 * @param color
 * Glow color.
 * @param speed
 * Animation speed of the glow.
 * @param state
 * Current animation state of the glow.
 * @return
 * New surface.
 */
static SDL_Surface *
sprite_effect_glow(SDL_Surface *surface, const SDL_Color *color, double speed, double state) {
    SDL_Surface *tmp = SDL_CreateSurface(surface->w + SPRITE_GLOW_SIZE * 2,
                                         surface->h + SPRITE_GLOW_SIZE * 2,
                                         surface->format);
    if (tmp == NULL) {
        return NULL;
    }

#define GLOW_GRID_PIXEL_NONE 0 ///< No data.
#define GLOW_GRID_PIXEL_VISIBLE 1 ///< A visible pixel.
#define GLOW_GRID_PIXEL_GLOW 2 ///< Added glow pixel.
#define GLOW_GRID_PIXEL_OUTLINE 3 ///< Added glow outline pixel.

    /* Create a 2D grid representation of the sprite's pixel surface for
     * storing information about the processing state, such as which
     * coordinates contain visible pixels. */
    uint8_t *grid = xcalloc((size_t)tmp->w * (size_t)tmp->h, sizeof(*grid));

    for (int x = 0; x < surface->w; x++) {
        for (int y = 0; y < surface->h; y++) {
            Uint32 pixel = getpixel(surface, x, y);
            Uint32 color_key;
            if (SDL_GetSurfaceColorKey(surface, &color_key) && pixel == color_key) {
                /* Transparent pixel. */
                continue;
            }

            putpixel(tmp, x + SPRITE_GLOW_SIZE, y + SPRITE_GLOW_SIZE, pixel);

            Uint8 r, g, b, a;
            pixel_format_get_rgba(pixel, surface->format, &r, &g, &b, &a);
            if (a < 127) {
                /* Avoid outlining pixels with low alpha values, such as
                 * shadows or already existing glow effects. */
                continue;
            }

            int idx = tmp->w * (y + SPRITE_GLOW_SIZE) + (x + SPRITE_GLOW_SIZE);
            grid[idx] = GLOW_GRID_PIXEL_VISIBLE;
        }
    }

    /* Figure out the alpha value based on the animation speed and the current
     * animation state for a fade-out/pulsing effect. */
    speed = MAX(1.0, speed);
    state = MAX(1.0, state);
    double mod = (speed - state - speed / 2.0) / (speed / 2.0);
    Uint8 alpha = 200.0 * fabs(mod);

    /* It's much easier to work in HSV for this. */
    double rgb[3], hsv[3];
    rgb[0] = color->r / 255.0;
    rgb[1] = color->g / 255.0;
    rgb[2] = color->b / 255.0;
    colorspace_rgb2hsv(rgb, hsv);

    /* Create some random variations of the specified color. */
    Uint32 pixels[10];
    for (size_t i = 0; i < arraysize(pixels); i++) {
        double hsv2[3], rgb2[3];
        memcpy(&hsv2, hsv, sizeof(hsv2));
        hsv2[1] += (10 - rndm(1, 20)) * 0.01;
        hsv2[2] += (10 - rndm(1, 20)) * 0.01;
        hsv2[1] = MIN(1.0, MAX(0.0, hsv2[1]));
        hsv2[2] = MIN(1.0, MAX(0.0, hsv2[2]));
        colorspace_hsv2rgb(hsv2, rgb2);

        pixels[i] = pixel_format_map_rgba(tmp->format,
                                          rgb2[0] * 255.0,
                                          rgb2[1] * 255.0,
                                          rgb2[2] * 255.0,
                                          alpha);
    }

    hsv[1] += 0.10;
    hsv[1] = MIN(1.0, hsv[1]);
    hsv[2] -= 0.25;
    hsv[2] = MAX(0.0, hsv[2]);
    colorspace_hsv2rgb(hsv, rgb);

    /* Acquire the color to use for the glow's outline. */
    Uint32 edge_color = pixel_format_map_rgba(tmp->format,
                                              rgb[0] * 255.0,
                                              rgb[1] * 255.0,
                                              rgb[2] * 255.0,
                                              MAX(0, alpha - 25));

    /* Iterate the pixels in the sprite's surface. */
    for (int x = 0; x < tmp->w; x++) {
        for (int y = 0; y < tmp->h; y++) {
            if (grid[tmp->w * y + x] != GLOW_GRID_PIXEL_VISIBLE) {
                /* Transparent pixel, or an already processed one. */
                continue;
            }

            /* Scan adjacent pixels to see if there's a visible pixel. */
            bool has_neighbors = false;
            for (int dx = -1; dx <= 1 && !has_neighbors; dx++) {
                for (int dy = -1; dy <= 1; dy++) {
                    if (dx == 0 && dy == 0) {
                        /* Skip self */
                        continue;
                    }

                    int tx = x + dx;
                    int ty = y + dy;
                    if (tx < 0 || tx >= tmp->w || ty < 0 || ty >= tmp->h) {
                        continue;
                    }

                    if (grid[tmp->w * ty + tx] == GLOW_GRID_PIXEL_VISIBLE) {
                        has_neighbors = true;
                        break;
                    }
                }
            }

            if (!has_neighbors) {
                /* No visible neighboring pixels, move on. */
                continue;
            }

            /* Add glow pixels where applicable. */
            for (int off = 1; off <= 2; off++) {
                for (int dx = -1; dx <= 1; dx++) {
                    for (int dy = -1; dy <= 1; dy++) {
                        int tx = x + dx * off;
                        int ty = y + dy * off;
                        if (tx < 0 || tx >= tmp->w || ty < 0 || ty >= tmp->h) {
                            continue;
                        }

                        uint8_t *point = &grid[tmp->w * ty + tx];
                        /* Only adjust pixels that don't have a visible pixel,
                         * or if they have been added a glow outline before. */
                        if (*point == GLOW_GRID_PIXEL_NONE || *point == GLOW_GRID_PIXEL_OUTLINE) {
                            Uint32 pixel;
                            if (off == 1) {
                                /* Glow pixel processing. */
                                pixel = pixels[rndm(0, arraysize(pixels) - 1)];
                                *point = GLOW_GRID_PIXEL_GLOW;
                            } else {
                                /* Glow outline pixel processing. */
                                pixel = edge_color;
                                *point = GLOW_GRID_PIXEL_OUTLINE;
                            }

                            putpixel(tmp, tx, ty, pixel);
                        }
                    }
                }
            }
        }
    }

    free(grid);

    SDL_Surface *ret = surface_to_display_alpha(tmp);
    SDL_DestroySurface(tmp);
    return ret;

#undef GLOW_GRID_PIXEL_NONE
#undef GLOW_GRID_PIXEL_VISIBLE
#undef GLOW_GRID_PIXEL_GLOW
#undef GLOW_GRID_PIXEL_OUTLINE
}

/**
 * Create a new sprite surface based on 'surface', applying the specified
 * 'effects'.
 *
 * @param surface
 * Surface to use as the base.
 * @param effects
 * Effects to apply.
 * @return
 * New surface, NULL on failure.
 */
SDL_Surface *sprite_effects_create(SDL_Surface *surface, const sprite_effects_t *effects) {
#define FREE_TMP_SURFACE()           \
    do {                             \
        if (tmp != NULL) {           \
            SDL_DestroySurface(tmp); \
        }                            \
        tmp = surface;               \
    } while (0)

    SDL_Surface *tmp = NULL;

    if (BIT_QUERY(effects->flags, SPRITE_FLAG_EFFECTS)) {
        surface = effect_sprite_overlay(surface);
        if (surface == NULL) {
            goto done;
        }

        FREE_TMP_SURFACE();
    }

    if (BIT_QUERY(effects->flags, SPRITE_FLAG_DARK)) {
        surface = surface_to_display_alpha(surface);
        if (surface == NULL) {
            goto done;
        }

        if (!surface_darken_preserve_alpha(surface, dark_alpha[effects->dark_level])) {
            SDL_DestroySurface(surface);
            surface = NULL;
            goto done;
        }
        FREE_TMP_SURFACE();
    } else if (BIT_QUERY(effects->flags, SPRITE_FLAG_FOW)) {
        surface = sprite_effect_fow(surface);
        if (surface == NULL) {
            goto done;
        }

        FREE_TMP_SURFACE();
    } else if (BIT_QUERY(effects->flags, SPRITE_FLAG_RED)) {
        surface = sprite_effect_red(surface);
        if (surface == NULL) {
            goto done;
        }

        FREE_TMP_SURFACE();
    } else if (BIT_QUERY(effects->flags, SPRITE_FLAG_GRAY)) {
        surface = sprite_effect_gray(surface);
        if (surface == NULL) {
            goto done;
        }

        FREE_TMP_SURFACE();
    }

    /* Apply tile-stretching. */
    if (effects->stretch != 0) {
        Sint8 n = (effects->stretch >> 24) & 0xFF;
        Sint8 e = (effects->stretch >> 16) & 0xFF;
        Sint8 w = (effects->stretch >> 8) & 0xFF;
        Sint8 s = effects->stretch & 0xFF;

        surface = tile_stretch(surface, n, e, s, w);
        if (surface == NULL) {
            goto done;
        }

        FREE_TMP_SURFACE();
    }

    /* Apply zoom and/or rotate effects. */
    if ((effects->zoom_x != 0 && effects->zoom_x != 100) ||
        (effects->zoom_y != 0 && effects->zoom_y != 100) || effects->rotate != 0) {
        bool smooth;
        /* Figure out whether to use smoothing. */
        if (effects->rotate == 0 && (effects->zoom_x == 0 || abs(effects->zoom_x) == 100) &&
            (effects->zoom_y == 0 || abs(effects->zoom_y) == 100)) {
            smooth = false;
        } else {
            smooth = setting_get_int(OPT_CAT_CLIENT, OPT_ZOOM_SMOOTH);
        }

        double zoom_x = effects->zoom_x != 0 ? effects->zoom_x / 100.0 : 1.0;
        double zoom_y = effects->zoom_y != 0 ? effects->zoom_y / 100.0 : 1.0;
        surface = rotozoomSurfaceXY(surface, effects->rotate, zoom_x, zoom_y, smooth);
        if (surface == NULL) {
            goto done;
        }

        FREE_TMP_SURFACE();
    }

    /* Apply glow effects. */
    if (effects->glow[0] != '\0') {
        SDL_Color color;
        if (text_color_parse(effects->glow, &color)) {
            surface = sprite_effect_glow(surface, &color, effects->glow_speed, effects->glow_state);
            if (surface == NULL) {
                goto done;
            }

            FREE_TMP_SURFACE();
        }
    }

    if (effects->outline[0] != '\0') {
        SDL_Color color;
        if (text_color_parse(effects->outline, &color)) {
            surface = sprite_outline_create(surface, &color);
            if (surface == NULL) {
                goto done;
            }

            FREE_TMP_SURFACE();
        }
    }

    /* Alpha transparency. */
    if (effects->alpha != 0) {
        surface = surface_to_display_alpha(surface);
        if (surface == NULL) {
            goto done;
        }

        surface_set_alpha(surface, effects->alpha);
        FREE_TMP_SURFACE();
    }

    SOFT_ASSERT_RC(tmp != NULL, NULL, "Generated NULL surface!");

done:
    return tmp;
#undef FREE_TMP_SURFACE
}

/**
 * Render the specified surface.
 *
 * @param surface
 * Surface on which to render.
 * @param x
 * X rendering position.
 * @param y
 * Y rendering position.
 * @param srcrect
 * Limit which parts of the source surface to render. Can be NULL.
 * @param src
 * Source surface to render.
 */
void surface_show(SDL_Surface *surface, int x, int y, SDL_Rect *srcrect, SDL_Surface *src) {
    SDL_Rect dstrect;
    dstrect.x = x;
    dstrect.y = y;
    SDL_BlitSurface(src, srcrect, surface, &dstrect);
}

/**
 * Render the specified surface until the specified 'box' is completely filled.
 *
 * Used for rendering tile-able textures.
 *
 * @param surface
 * Surface on which to render.
 * @param x
 * X rendering position.
 * @param y
 * Y rendering position.
 * @param srcrect
 * Limit which parts of the source surface to render. Can be NULL.
 * @param src
 * Source surface to render.
 * @param box
 * Specifies maximum width and height to render.
 */
void surface_show_fill(SDL_Surface *surface,
                       int x,
                       int y,
                       SDL_Rect *srcsize,
                       SDL_Surface *src,
                       SDL_Rect *box) {
    int w = srcsize != NULL ? srcsize->w : src->w;
    int h = srcsize != NULL ? srcsize->h : src->h;
    for (int tx = 0; tx < box->w; tx += w) {
        for (int ty = 0; ty < box->h; ty += h) {
            SDL_Rect srcrect;
            srcrect.x = srcsize ? MAX(0, srcsize->x) : 0;
            srcrect.y = srcsize ? MAX(0, srcsize->y) : 0;
            srcrect.w = MIN(w, box->w - tx);
            srcrect.h = MIN(h, box->h - ty);
            surface_show(surface, x + tx, y + ty, &srcrect, src);
        }
    }
}

/**
 * Render a surface, applying the specified effects.
 *
 * @param surface
 * Surface on which to render.
 * @param x
 * X rendering position.
 * @param y
 * Y rendering position.
 * @param srcrect
 * Limit which parts of the source surface to render. Can be NULL.
 * @param src
 * Source surface to render.
 * @param effects
 * Effects to apply.
 */
void surface_show_effects(SDL_Surface *surface,
                          int x,
                          int y,
                          SDL_Rect *srcrect,
                          SDL_Surface *src,
                          const sprite_effects_t *effects) {
    HARD_ASSERT(surface != NULL);
    bool temporary_effect_surface = false;

    if (src == NULL) {
        return;
    }

    if (effects != NULL && SPRITE_EFFECTS_NEED_RENDERING(effects)) {
        /* Maximum darkness; do not render at all. */
        if (BIT_QUERY(effects->flags, SPRITE_FLAG_DARK) && effects->dark_level == DARK_LEVELS) {
            return;
        }

        /* Construct a cache entry string. */
        char name[HUGE_BUF];
        snprintf(VS(name),
                 "%p;%u;%u;%s;%u;%u;%d;%d;%d;%s;%s;%u;%u",
                 src,
                 effects->flags,
                 effects->dark_level,
                 effect_overlay_identifier(),
                 effects->alpha,
                 effects->stretch,
                 effects->zoom_x,
                 effects->zoom_y,
                 effects->rotate,
                 effects->glow,
                 effects->outline,
                 effects->glow_speed,
                 effects->glow_state);

        /* Try to find the sprite we need in the cache, otherwise,
         * render it out and add it to the cache. */
        SDL_Surface *old_src = src;
        sprite_cache_t *cache = sprite_cache_find(name);
        if (cache != NULL) {
            src = cache->surface;
        } else {
            SDL_Surface *tmp = sprite_effects_create(src, effects);
            if (tmp != NULL) {
                src = tmp;

                cache = sprite_cache_create(name);
                cache->source = old_src;
                cache->surface = src;
                if (!sprite_cache_add(cache)) {
                    free(cache->name);
                    free(cache);
                    temporary_effect_surface = true;
                }
            }
        }

        if (effects->stretch != 0) {
            y -= src->h - old_src->h;
        }

        if (effects->glow[0] != '\0') {
            y -= SPRITE_GLOW_SIZE;
            x -= SPRITE_GLOW_SIZE;
        }

        if (effects->outline[0] != '\0') {
            y -= SPRITE_GLOW_SIZE;
            x -= SPRITE_GLOW_SIZE;
        }
    }

    if (effects != NULL && BIT_QUERY(effects->flags, SPRITE_FLAG_SMOOTH_DARK)) {
        lighting_show_surface(surface,
                              x,
                              y,
                              srcrect,
                              src,
                              effects->smooth_dark_y,
                              LIGHTING_SURFACE_STRUCTURE);
    } else if (effects != NULL && BIT_QUERY(effects->flags, SPRITE_FLAG_SMOOTH_DARK_SURFACE)) {
        if (effects->smooth_dark_constant) {
            lighting_show_surface_constant(surface,
                                           x,
                                           y,
                                           srcrect,
                                           src,
                                           effects->smooth_dark_scalar,
                                           effects->smooth_dark_rgb);
        } else {
            lighting_show_surface(surface, x, y, srcrect, src, 0, LIGHTING_SURFACE_PROJECTED);
        }
    } else {
        surface_show(surface, x, y, srcrect, src);
    }

    if (temporary_effect_surface) {
        lighting_invalidate_surface(src);
        SDL_DestroySurface(src);
    }
}

/**
 * Put a pixel value to the specified X/Y position on an SDL surface.
 *
 * @param surface
 * The surface.
 * @param x
 * X position.
 * @param y
 * Y position.
 * @param pixel
 * Pixel to put.
 */
void putpixel(SDL_Surface *surface, int x, int y, Uint32 pixel) {
    const SDL_PixelFormatDetails *details = SDL_GetPixelFormatDetails(surface->format);
    HARD_ASSERT(details != NULL);
    int bpp = details->bytes_per_pixel;
    /* The address to the pixel we want to set. */
    Uint8 *p = (Uint8 *)surface->pixels + y * surface->pitch + x * bpp;

    switch (bpp) {
        case 1:
            *p = pixel;
            break;

        case 2:
            *(Uint16 *)p = pixel;
            break;

        case 3:
            if (SDL_BYTEORDER == SDL_BIG_ENDIAN) {
                p[0] = (pixel >> 16) & 0xff;
                p[1] = (pixel >> 8) & 0xff;
                p[2] = pixel & 0xff;
            } else {
                p[0] = pixel & 0xff;
                p[1] = (pixel >> 8) & 0xff;
                p[2] = (pixel >> 16) & 0xff;
            }

            break;

        case 4:
            *(Uint32 *)p = pixel;
            break;
    }
}

/**
 * Calculate the left border in the surface - this is the position of
 * the first pixel from the left that that does not match 'color'.
 *
 * @param surface
 * Surface.
 * @param[out] pos
 * Where to store the position.
 * @param color
 * Color to check for.
 * @return
 * True if the border was found, false otherwise.
 */
static bool surface_border_get_left(SDL_Surface *surface, int *pos, uint32_t color) {
    for (int x = 0; x < surface->w; x++) {
        for (int y = 0; y < surface->h; y++) {
            if (getpixel(surface, x, y) != color) {
                *pos = x;
                return true;
            }
        }
    }

    return false;
}

/**
 * Calculate the right border in the surface - this is the position of
 * the first pixel from the right that that does not match 'color'.
 *
 * @param surface
 * Surface.
 * @param[out] pos
 * Where to store the position.
 * @param color
 * Color to check for.
 * @return
 * True if the border was found, false otherwise.
 */
static bool surface_border_get_right(SDL_Surface *surface, int *pos, uint32_t color) {
    for (int x = surface->w - 1; x >= 0; x--) {
        for (int y = 0; y < surface->h; y++) {
            if (getpixel(surface, x, y) != color) {
                *pos = (surface->w - 1) - x;
                return true;
            }
        }
    }

    return false;
}

/**
 * Calculate the top border in the surface - this is the position of
 * the first pixel from the top that that does not match 'color'.
 *
 * @param surface
 * Surface.
 * @param[out] pos
 * Where to store the position.
 * @param color
 * Color to check for.
 * @return
 * True if the border was found, false otherwise.
 */
static bool surface_border_get_top(SDL_Surface *surface, int *pos, uint32_t color) {
    for (int y = 0; y < surface->h; y++) {
        for (int x = 0; x < surface->w; x++) {
            if (getpixel(surface, x, y) != color) {
                *pos = y;
                return true;
            }
        }
    }

    return false;
}

/**
 * Calculate the bottom border in the surface - this is the position of
 * the first pixel from the bottom that that does not match 'color'.
 *
 * @param surface
 * Surface.
 * @param[out] pos
 * Where to store the position.
 * @param color
 * Color to check for.
 * @return
 * True if the border was found, false otherwise.
 */
static bool surface_border_get_bottom(SDL_Surface *surface, int *pos, uint32_t color) {
    for (int y = surface->h - 1; y >= 0; y--) {
        for (int x = 0; x < surface->w; x++) {
            if (getpixel(surface, x, y) != color) {
                *pos = (surface->h - 1) - y;
                return true;
            }
        }
    }

    return false;
}

/**
 * Get borders from SDL_surface. The borders indicate the first pixel
 * from the border's side that does not match 'color'.
 *
 * @param surface
 * Surface to get borders from.
 * @param[out] top
 * Where to store the top border.
 * @param[out] bottom
 * Where to store the bottom border.
 * @param[out] left
 * Where to store the left border.
 * @param[out] right
 * Where to store the right border.
 * @param color
 * Color to check for.
 * @return
 * 1 if the borders were found, 0 otherwise (image is all filled with 'color'
 * color).
 */
int surface_borders_get(SDL_Surface *surface,
                        int *top,
                        int *bottom,
                        int *left,
                        int *right,
                        uint32_t color) {
    *top = 0;
    *bottom = 0;
    *left = 0;
    *right = 0;

    /* If the border was not found, it means the surface is completely
     * filled with 'color' color. */
    if (!surface_border_get_top(surface, top, color)) {
        return 0;
    }

    surface_border_get_bottom(surface, bottom, color);
    surface_border_get_left(surface, left, color);
    surface_border_get_right(surface, right, color);

    return 1;
}

/**
 * Pans the surface.
 *
 * @param surface
 * Surface.
 * @param box
 * Coordinates.
 */
void surface_pan(SDL_Surface *surface, SDL_Rect *box) {
    if (box->x >= surface->w - box->w) {
        box->x = (Sint16)(surface->w - box->w);
    }

    if (box->x < 0) {
        box->x = 0;
    }

    if (box->y >= surface->h - box->h) {
        box->y = (Sint16)(surface->h - box->h);
    }

    if (box->y < 0) {
        box->y = 0;
    }
}

/**
 * Draw a border frame.
 *
 * @param surface
 * Surface to draw on.
 * @param x
 * X position.
 * @param y
 * Y position.
 * @param w
 * Width of the frame.
 * @param h
 * Height of the frame.
 */
void draw_frame(SDL_Surface *surface, int x, int y, int w, int h) {
    SDL_Rect box;

    box.x = x;
    box.y = y;
    box.h = h;
    box.w = 1;
    SDL_FillSurfaceRect(surface, &box, pixel_format_map_rgb(surface->format, 0x60, 0x60, 0x60));
    box.x = x + w;
    box.h++;
    SDL_FillSurfaceRect(surface, &box, pixel_format_map_rgb(surface->format, 0x55, 0x55, 0x55));
    box.x = x;
    box.y += h;
    box.w = w;
    box.h = 1;
    SDL_FillSurfaceRect(surface, &box, pixel_format_map_rgb(surface->format, 0x60, 0x60, 0x60));
    box.x++;
    box.y = y;
    SDL_FillSurfaceRect(surface, &box, pixel_format_map_rgb(surface->format, 0x55, 0x55, 0x55));
}

/**
 * Create a border around the specified coordinates.
 *
 * @param surface
 * Surface to use.
 * @param x
 * X start of the border.
 * @param y
 * Y start of the border.
 * @param w
 * Maximum border width.
 * @param h
 * Maximum border height.
 * @param color
 * Color to use for the border.
 * @param size
 * Border's size.
 */
void border_create(SDL_Surface *surface, int x, int y, int w, int h, int color, int size) {
    SDL_Rect box;

    /* Left border. */
    box.x = x;
    box.y = y;
    box.h = h;
    box.w = size;
    SDL_FillSurfaceRect(surface, &box, color);

    /* Right border. */
    box.x = x + w - size;
    SDL_FillSurfaceRect(surface, &box, color);

    /* Top border. */
    box.x = x + size;
    box.y = y;
    box.w = w - size * 2;
    box.h = size;
    SDL_FillSurfaceRect(surface, &box, color);

    /* Bottom border. */
    box.y = y + h - size;
    SDL_FillSurfaceRect(surface, &box, color);
}

/**
 * Render a line (essentially a rectangle) of the specified width/height and
 * color.
 *
 * @param surface
 * Surface to render on.
 * @param x
 * Starting X coordinate.
 * @param y
 * Starting Y coordinate.
 * @param w
 * Width of the line.
 * @param h
 * Height of the line.
 * @param color
 * Color of the line.
 */
void border_create_line(SDL_Surface *surface, int x, int y, int w, int h, uint32_t color) {
    SDL_Rect dst;

    dst.x = x;
    dst.y = y;
    dst.w = w;
    dst.h = h;
    SDL_FillSurfaceRect(surface, &dst, color);
}

/**
 * Render a border of the specified SDL color and thickness.
 *
 * @param surface
 * Surface to render on.
 * @param coords
 * Coordinates to render at.
 * @param thickness
 * Border thickness.
 * @param color
 * Border color.
 */
void border_create_sdl_color(SDL_Surface *surface,
                             SDL_Rect *coords,
                             int thickness,
                             SDL_Color *color) {
    uint32_t color_mapped = pixel_format_map_rgb(surface->format, color->r, color->g, color->b);

    BORDER_CREATE_TOP(surface, coords->x, coords->y, coords->w, coords->h, color_mapped, thickness);
    BORDER_CREATE_BOTTOM(surface,
                         coords->x,
                         coords->y,
                         coords->w,
                         coords->h,
                         color_mapped,
                         thickness);
    BORDER_CREATE_LEFT(surface,
                       coords->x,
                       coords->y,
                       coords->w,
                       coords->h,
                       color_mapped,
                       thickness);
    BORDER_CREATE_RIGHT(surface,
                        coords->x,
                        coords->y,
                        coords->w,
                        coords->h,
                        color_mapped,
                        thickness);
}

/**
 * Render a border of the specified color and thickness.
 *
 * @param surface
 * Surface to render on.
 * @param coords
 * Coordinates to render at.
 * @param thickness
 * Border thickness.
 * @param color_notation
 * Border color, eg, "ff0000".
 */
void border_create_color(SDL_Surface *surface,
                         SDL_Rect *coords,
                         int thickness,
                         const char *color_notation) {
    SDL_Color color;
    if (!text_color_parse(color_notation, &color)) {
        LOG(ERROR, "Invalid color: %s", color_notation);
        return;
    }

    border_create_sdl_color(surface, coords, thickness, &color);
}

/**
 * Render a border using the specified texture.
 *
 * The texture should be tile-able.
 *
 * @param surface
 * Surface to render on.
 * @param coords
 * Coordinates to render at.
 * @param thickness
 * Border thickness.
 * @param texture
 * Border texture.
 */
void border_create_texture(SDL_Surface *surface,
                           SDL_Rect *coords,
                           int thickness,
                           SDL_Surface *texture) {
    SDL_Rect box;

    box.w = coords->w;
    box.h = thickness;
    surface_show_fill(surface, coords->x, coords->y, NULL, texture, &box);
    surface_show_fill(surface, coords->x, coords->y + coords->h - thickness, NULL, texture, &box);

    box.w = thickness;
    box.h = coords->h;
    surface_show_fill(surface, coords->x, coords->y, NULL, texture, &box);
    surface_show_fill(surface, coords->x + coords->w - thickness, coords->y, NULL, texture, &box);
}

/**
 * Create a rectangle of the specified size and color.
 *
 * @param surface
 * Surface to render on.
 * @param x
 * X coordinate to render at.
 * @param y
 * Y coordinate to render at.
 * @param w
 * Width of the rectangle.
 * @param h
 * Height of the rectangle.
 * @param color_notation
 * Color of the rectangle, eg, "ff0000".
 */
void rectangle_create(SDL_Surface *surface,
                      int x,
                      int y,
                      int w,
                      int h,
                      const char *color_notation) {
    SDL_Color color;
    if (!text_color_parse(color_notation, &color)) {
        LOG(BUG, "Invalid color: %s", color_notation);
        return;
    }

    border_create_line(surface,
                       x,
                       y,
                       w,
                       h,
                       pixel_format_map_rgb(surface->format, color.r, color.g, color.b));
}

/**
 * Sets the whole-surface alpha modulation used when the surface is blitted.
 * Per-pixel alpha remains intact and is multiplied by this value.
 *
 * @param surface
 * Surface to change alpha value of.
 * @param alpha
 * Alpha value to set.
 */
void surface_set_alpha(SDL_Surface *surface, uint8_t alpha) {
    HARD_ASSERT(surface != NULL);

    if (!SDL_SetSurfaceAlphaMod(surface, alpha) ||
        !SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_BLEND)) {
        LOG(ERROR, "Could not set surface alpha: %s", SDL_GetError());
    }
}

/**
 * Checks whether the given coordinates are within the specified polygon.
 *
 * The arrays corners_x/corners_y should contain every single corner point of
 * the polygon that you want to test.
 *
 * @param x
 * X coordinate.
 * @param y
 * Y coordinate.
 * @param corners_x
 * Array of X corner coordinates.
 * @param corners_y
 * Array of Y corner coordinates.
 * @param corners_num
 * Number of corner coordinate entries.
 * @return
 * 1 if the coordinates are in the polygon, 0 otherwise.
 */
int polygon_check_coords(double x,
                         double y,
                         double corners_x[],
                         double corners_y[],
                         int corners_num) {
    int j = corners_num - 1;
    int odd_nodes = 0;

    for (int i = 0; i < corners_num; i++) {
        if (((corners_y[i] < y && corners_y[j] >= y) || (corners_y[j] < y && corners_y[i] >= y)) &&
            (corners_x[i] <= x || corners_x[j] <= x)) {
            odd_nodes ^= (corners_x[i] + (y - corners_y[i]) / (corners_y[j] - corners_y[i]) *
                                             (corners_x[j] - corners_x[i]) <
                          x);
        }

        j = i;
    }

    return odd_nodes;
}
