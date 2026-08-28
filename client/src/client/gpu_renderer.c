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

/**
 * @file
 * Mandatory SDL_GPU-backed renderer device and retained upload cache.
 */

#include <global.h>

#define GPU_RENDERER_SURFACE_GENERATION_PROPERTY "atrinik.gpu.surface_generation"
#define GPU_RENDERER_CANVAS_PROPERTY "atrinik.gpu.canvas"
#define GPU_RENDERER_ATLAS_SIZE 2048
#define GPU_RENDERER_ATLAS_ENTRY_LIMIT 512

typedef struct gpu_surface_texture {
    SDL_Surface *surface;
    SDL_Texture *texture;
    Uint64 generation;
    size_t bytes;
    SDL_FRect atlas_source;
    bool atlased;
    struct gpu_surface_texture *next;
} gpu_surface_texture_t;

typedef struct gpu_texture_atlas {
    SDL_Texture *texture;
    int next_x;
    int next_y;
    int row_height;
    size_t bytes;
    struct gpu_texture_atlas *next;
} gpu_texture_atlas_t;

typedef struct gpu_canvas {
    SDL_Surface *surface;
    SDL_Texture *texture;
    size_t bytes;
    struct gpu_canvas *next;
} gpu_canvas_t;

static SDL_Renderer *renderer;
static SDL_GPUDevice *device;
static SDL_Texture *frame_target;
static size_t frame_target_bytes;
static gpu_surface_texture_t *surface_textures;
static gpu_texture_atlas_t *texture_atlases;
static gpu_canvas_t *canvases;
static Uint64 next_surface_generation = 1;
static gpu_renderer_statistics_t statistics;
static char backend[32];
static char device_name[256];
static char driver_name[256];
static char driver_version[256];
static bool frame_failed;
static bool recreation_requested;

static bool gpu_renderer_draw_color(Uint8 red, Uint8 green, Uint8 blue, Uint8 alpha);

static bool gpu_renderer_frame_result(bool success) {
    frame_failed |= !success;
    return success;
}

static void gpu_renderer_copy_property(char *destination,
                                       size_t size,
                                       SDL_PropertiesID properties,
                                       const char *name) {
    const char *value = SDL_GetStringProperty(properties, name, "unavailable");
    snprintf(destination, size, "%s", value != NULL && *value != '\0' ? value : "unavailable");
}

static bool gpu_renderer_backend_supported(const char *name) {
    return name != NULL && (strcmp(name, "vulkan") == 0 || strcmp(name, "direct3d12") == 0 ||
                            strcmp(name, "metal") == 0);
}

static bool gpu_renderer_formats_supported(SDL_GPUDevice *candidate) {
    const SDL_GPUTextureUsageFlags usage =
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    return SDL_GPUTextureSupportsFormat(candidate,
                                        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                        SDL_GPU_TEXTURETYPE_2D,
                                        usage) &&
           SDL_GPUTextureSupportsFormat(candidate,
                                        SDL_GPU_TEXTUREFORMAT_R8_UINT,
                                        SDL_GPU_TEXTURETYPE_2D,
                                        usage);
}

static void gpu_renderer_surface_texture_destroy(gpu_surface_texture_t *entry) {
    if (entry->texture != NULL && !entry->atlased) {
        SDL_DestroyTexture(entry->texture);
        statistics.resource_destructions++;
        statistics.retained_bytes -= entry->bytes;
    }
    free(entry);
}

static void gpu_renderer_texture_atlases_destroy(void) {
    while (texture_atlases != NULL) {
        gpu_texture_atlas_t *atlas = texture_atlases;
        texture_atlases = atlas->next;
        SDL_DestroyTexture(atlas->texture);
        statistics.resource_destructions++;
        statistics.retained_bytes -= atlas->bytes;
        free(atlas);
    }
}

static void gpu_renderer_surface_textures_destroy(void) {
    while (surface_textures != NULL) {
        gpu_surface_texture_t *entry = surface_textures;
        surface_textures = entry->next;
        gpu_renderer_surface_texture_destroy(entry);
    }
}

static void gpu_renderer_canvas_texture_destroy(gpu_canvas_t *canvas) {
    if (canvas->texture == NULL) {
        return;
    }
    SDL_DestroyTexture(canvas->texture);
    canvas->texture = NULL;
    statistics.resource_destructions++;
    statistics.retained_bytes -= canvas->bytes;
}

static void SDLCALL gpu_renderer_canvas_cleanup(void *userdata, void *value) {
    (void)userdata;
    gpu_canvas_t *canvas = value;
    gpu_canvas_t **link = &canvases;
    while (*link != NULL && *link != canvas) {
        link = &(*link)->next;
    }
    if (*link == canvas) {
        *link = canvas->next;
    }
    gpu_renderer_canvas_texture_destroy(canvas);
    free(canvas);
}

static void gpu_renderer_canvas_textures_destroy(void) {
    for (gpu_canvas_t *canvas = canvases; canvas != NULL; canvas = canvas->next) {
        gpu_renderer_canvas_texture_destroy(canvas);
    }
}

static void gpu_renderer_device_destroy(void) {
    gpu_map_renderer_destroy();
    gpu_renderer_canvas_textures_destroy();
    gpu_renderer_surface_textures_destroy();
    gpu_renderer_texture_atlases_destroy();
    if (frame_target != NULL) {
        SDL_DestroyTexture(frame_target);
        frame_target = NULL;
        statistics.resource_destructions++;
        statistics.retained_bytes -= frame_target_bytes;
        frame_target_bytes = 0;
    }
    if (renderer != NULL) {
        SDL_DestroyRenderer(renderer);
        renderer = NULL;
    }
    if (device != NULL) {
        SDL_DestroyGPUDevice(device);
        device = NULL;
    }
    backend[0] = '\0';
    device_name[0] = '\0';
    driver_name[0] = '\0';
    driver_version[0] = '\0';
}

bool gpu_renderer_create(SDL_Window *window) {
    HARD_ASSERT(window != NULL);
    gpu_renderer_device_destroy();

    SDL_PropertiesID properties = SDL_CreateProperties();
    if (properties == 0) {
        return false;
    }
    bool configured =
        SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN, true) &&
        SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXIL_BOOLEAN, true) &&
        SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN, true) &&
        SDL_SetBooleanProperty(properties,
                               SDL_PROP_GPU_DEVICE_CREATE_VULKAN_REQUIRE_HARDWARE_ACCELERATION_BOOLEAN,
                               true);
    if (configured) {
        device = SDL_CreateGPUDeviceWithProperties(properties);
    }
    SDL_DestroyProperties(properties);
    if (!configured || device == NULL) {
        gpu_renderer_device_destroy();
        return false;
    }

    const char *selected_backend = SDL_GetGPUDeviceDriver(device);
    if (!gpu_renderer_backend_supported(selected_backend) || !gpu_renderer_formats_supported(device)) {
        SDL_SetError("GPU renderer requires Vulkan, Direct3D 12, or Metal with RGBA8 and R8_UINT render targets");
        gpu_renderer_device_destroy();
        return false;
    }

    renderer = SDL_CreateGPURenderer(device, window);
    if (renderer == NULL || SDL_GetGPURendererDevice(renderer) != device) {
        gpu_renderer_device_destroy();
        return false;
    }
    if (!SDL_SetDefaultTextureScaleMode(renderer, SDL_SCALEMODE_NEAREST) ||
        !gpu_map_renderer_create(device, renderer)) {
        gpu_renderer_device_destroy();
        return false;
    }

    snprintf(backend, sizeof(backend), "%s", selected_backend);
    SDL_PropertiesID device_properties = SDL_GetGPUDeviceProperties(device);
    gpu_renderer_copy_property(device_name,
                               sizeof(device_name),
                               device_properties,
                               SDL_PROP_GPU_DEVICE_NAME_STRING);
    gpu_renderer_copy_property(driver_name,
                               sizeof(driver_name),
                               device_properties,
                               SDL_PROP_GPU_DEVICE_DRIVER_NAME_STRING);
    gpu_renderer_copy_property(driver_version,
                               sizeof(driver_version),
                               device_properties,
                               SDL_PROP_GPU_DEVICE_DRIVER_VERSION_STRING);
    return true;
}

bool gpu_renderer_recover(SDL_Window *window) {
    bool succeeded = gpu_renderer_create(window);
    gpu_renderer_statistics_recovery(succeeded);
    return succeeded;
}

void gpu_renderer_recreation_request(void) {
    recreation_requested = true;
}

bool gpu_renderer_recreation_take_request(void) {
    bool requested = recreation_requested;
    recreation_requested = false;
    return requested;
}

void gpu_renderer_destroy(void) {
    gpu_renderer_device_destroy();
}

bool gpu_renderer_ready(void) {
    return renderer != NULL && device != NULL;
}

SDL_Renderer *gpu_renderer_sdl(void) {
    return renderer;
}

SDL_GPUDevice *gpu_renderer_device(void) {
    return device;
}

const char *gpu_renderer_backend(void) {
    return backend;
}

const char *gpu_renderer_device_name(void) {
    return device_name;
}

const char *gpu_renderer_driver_name(void) {
    return driver_name;
}

const char *gpu_renderer_driver_version(void) {
    return driver_version;
}

bool gpu_renderer_output_size(int *width, int *height) {
    return renderer != NULL && SDL_GetRenderOutputSize(renderer, width, height);
}

static bool gpu_renderer_frame_target_create(void) {
    int width, height;
    if (renderer == NULL || !SDL_GetRenderOutputSize(renderer, &width, &height) || width <= 0 ||
        height <= 0) {
        return false;
    }
    if (frame_target != NULL) {
        float current_width, current_height;
        if (SDL_GetTextureSize(frame_target, &current_width, &current_height) &&
            (int)current_width == width && (int)current_height == height) {
            return true;
        }
        SDL_DestroyTexture(frame_target);
        frame_target = NULL;
        statistics.resource_destructions++;
        statistics.retained_bytes -= frame_target_bytes;
        frame_target_bytes = 0;
    }
    frame_target = SDL_CreateTexture(renderer,
                                     SDL_PIXELFORMAT_RGBA32,
                                     SDL_TEXTUREACCESS_TARGET,
                                     width,
                                     height);
    if (frame_target == NULL || !SDL_SetTextureScaleMode(frame_target, SDL_SCALEMODE_NEAREST) ||
        !SDL_SetTextureBlendMode(frame_target, SDL_BLENDMODE_NONE)) {
        SDL_DestroyTexture(frame_target);
        frame_target = NULL;
        return false;
    }
    frame_target_bytes = (size_t)width * (size_t)height * 4U;
    statistics.resource_creations++;
    statistics.retained_bytes += frame_target_bytes;
    statistics.peak_retained_bytes = MAX(statistics.peak_retained_bytes,
                                         statistics.retained_bytes);
    return true;
}

bool gpu_renderer_begin_frame(void) {
    frame_failed = false;
    if (!gpu_renderer_frame_target_create() || !SDL_SetRenderTarget(renderer, frame_target) ||
        !SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE)) {
        return gpu_renderer_frame_result(false);
    }
    return gpu_renderer_frame_result(SDL_RenderClear(renderer));
}

bool gpu_renderer_present(void) {
    if (renderer == NULL || frame_failed) {
        return false;
    }
    uint64_t started = gpu_renderer_timing_begin();
    bool result = SDL_SetRenderTarget(renderer, NULL) &&
                  SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE) &&
                  SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE) &&
                  SDL_RenderClear(renderer) && SDL_RenderTexture(renderer, frame_target, NULL, NULL) &&
                  SDL_RenderPresent(renderer);
    statistics.draws++;
    gpu_renderer_timing_end(GPU_RENDERER_TIMING_PRESENT_WAIT, started);
    return gpu_renderer_frame_result(result);
}

bool gpu_renderer_frame_valid(void) {
    return renderer != NULL && !frame_failed;
}

bool gpu_renderer_map_begin(int width, int height) {
    return gpu_renderer_frame_result(gpu_map_renderer_begin(width, height));
}

void gpu_renderer_map_set_owner(uint8_t owner) {
    gpu_map_renderer_set_owner(owner);
}

void gpu_renderer_map_light_quad(uint8_t owner, const lighting_vertex_t vertices[4]) {
    gpu_map_renderer_light_quad(owner, vertices);
}

bool gpu_renderer_map_end(void) {
    return gpu_renderer_frame_result(gpu_map_renderer_end());
}

bool gpu_renderer_draw_map(float x, float y, float width, float height) {
    SDL_Texture *map_target = gpu_map_renderer_texture();
    if (renderer == NULL || map_target == NULL || width <= 0.0f || height <= 0.0f) {
        return false;
    }
    SDL_FRect destination = {x, y, width, height};
    statistics.draws++;
    return gpu_renderer_frame_result(
        SDL_SetTextureScaleMode(map_target,
                                zoom_filter_to_scale_mode(
                                    setting_get_int(OPT_CAT_CLIENT, OPT_ZOOM_FILTER))) &&
        SDL_RenderTexture(renderer, map_target, NULL, &destination));
}

static gpu_texture_atlas_t *gpu_renderer_texture_atlas_create(void) {
    gpu_texture_atlas_t *atlas = xcalloc(1, sizeof(*atlas));
    atlas->texture = SDL_CreateTexture(renderer,
                                       SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_TARGET,
                                       GPU_RENDERER_ATLAS_SIZE,
                                       GPU_RENDERER_ATLAS_SIZE);
    SDL_Texture *previous = SDL_GetRenderTarget(renderer);
    SDL_Rect previous_clip;
    bool previous_clip_enabled = SDL_RenderClipEnabled(renderer);
    if (previous_clip_enabled) {
        SDL_GetRenderClipRect(renderer, &previous_clip);
    }
    bool success = atlas->texture != NULL &&
                   SDL_SetTextureScaleMode(atlas->texture, SDL_SCALEMODE_NEAREST) &&
                   SDL_SetTextureBlendMode(atlas->texture, SDL_BLENDMODE_BLEND) &&
                   SDL_SetRenderTarget(renderer, atlas->texture) &&
                   SDL_SetRenderClipRect(renderer, NULL) &&
                   SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_TRANSPARENT) &&
                   SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE) &&
                   SDL_RenderClear(renderer) && SDL_SetRenderTarget(renderer, previous) &&
                   SDL_SetRenderClipRect(renderer,
                                         previous_clip_enabled ? &previous_clip : NULL);
    if (!success) {
        SDL_SetRenderTarget(renderer, previous);
        SDL_SetRenderClipRect(renderer, previous_clip_enabled ? &previous_clip : NULL);
        SDL_DestroyTexture(atlas->texture);
        free(atlas);
        return NULL;
    }
    atlas->bytes = (size_t)GPU_RENDERER_ATLAS_SIZE * GPU_RENDERER_ATLAS_SIZE * 4U;
    atlas->next = texture_atlases;
    texture_atlases = atlas;
    statistics.resource_creations++;
    statistics.retained_bytes += atlas->bytes;
    statistics.peak_retained_bytes = MAX(statistics.peak_retained_bytes,
                                         statistics.retained_bytes);
    return atlas;
}

static bool gpu_renderer_texture_atlas_place(SDL_Surface *surface,
                                             SDL_Texture **texture,
                                             SDL_FRect *source) {
    HARD_ASSERT(surface != NULL);
    HARD_ASSERT(texture != NULL);
    HARD_ASSERT(source != NULL);
    if (surface->w <= 0 || surface->h <= 0 || surface->w > GPU_RENDERER_ATLAS_ENTRY_LIMIT ||
        surface->h > GPU_RENDERER_ATLAS_ENTRY_LIMIT) {
        return false;
    }

    gpu_texture_atlas_t *atlas = texture_atlases;
    for (;;) {
        if (atlas == NULL) {
            atlas = gpu_renderer_texture_atlas_create();
            if (atlas == NULL) {
                return false;
            }
        }
        if (atlas->next_x + surface->w > GPU_RENDERER_ATLAS_SIZE) {
            atlas->next_x = 0;
            atlas->next_y += atlas->row_height;
            atlas->row_height = 0;
        }
        if (atlas->next_y + surface->h <= GPU_RENDERER_ATLAS_SIZE) {
            break;
        }
        atlas = atlas->next;
    }

    SDL_Texture *upload = SDL_CreateTextureFromSurface(renderer, surface);
    if (upload == NULL) {
        return false;
    }
    SDL_FRect destination = {(float)atlas->next_x,
                             (float)atlas->next_y,
                             (float)surface->w,
                             (float)surface->h};
    SDL_Texture *previous = SDL_GetRenderTarget(renderer);
    SDL_Rect previous_clip;
    bool previous_clip_enabled = SDL_RenderClipEnabled(renderer);
    if (previous_clip_enabled) {
        SDL_GetRenderClipRect(renderer, &previous_clip);
    }
    bool success = SDL_SetTextureBlendMode(upload, SDL_BLENDMODE_NONE) &&
                   SDL_SetRenderTarget(renderer, atlas->texture) &&
                   SDL_SetRenderClipRect(renderer, NULL) &&
                   SDL_RenderTexture(renderer, upload, NULL, &destination) &&
                   SDL_SetRenderTarget(renderer, previous) &&
                   SDL_SetRenderClipRect(renderer,
                                         previous_clip_enabled ? &previous_clip : NULL);
    SDL_DestroyTexture(upload);
    if (!success) {
        SDL_SetRenderTarget(renderer, previous);
        SDL_SetRenderClipRect(renderer, previous_clip_enabled ? &previous_clip : NULL);
        return false;
    }

    atlas->next_x += surface->w;
    atlas->row_height = MAX(atlas->row_height, surface->h);
    *texture = atlas->texture;
    *source = destination;
    return true;
}

static gpu_surface_texture_t *gpu_renderer_surface_texture(SDL_Surface *surface) {
    SDL_PropertiesID properties = SDL_GetSurfaceProperties(surface);
    Uint64 generation = SDL_GetNumberProperty(properties,
                                              GPU_RENDERER_SURFACE_GENERATION_PROPERTY,
                                              0);
    gpu_surface_texture_t **link = &surface_textures;
    while (*link != NULL) {
        gpu_surface_texture_t *entry = *link;
        if (entry->surface == surface) {
            if (generation != 0 && entry->generation == generation) {
                return entry;
            }
            *link = entry->next;
            gpu_renderer_surface_texture_destroy(entry);
            break;
        }
        link = &entry->next;
    }

    gpu_surface_texture_t *entry = xcalloc(1, sizeof(*entry));
    entry->atlased = gpu_renderer_texture_atlas_place(surface,
                                                      &entry->texture,
                                                      &entry->atlas_source);
    if (!entry->atlased) {
        entry->texture = SDL_CreateTextureFromSurface(renderer, surface);
        if (entry->texture == NULL) {
            free(entry);
            return NULL;
        }
        SDL_SetTextureScaleMode(entry->texture, SDL_SCALEMODE_NEAREST);
    }
    entry->surface = surface;
    entry->generation = next_surface_generation++;
    if (next_surface_generation == 0) {
        next_surface_generation = 1;
    }
    size_t upload_bytes = (size_t)surface->w * (size_t)surface->h * 4U;
    entry->bytes = entry->atlased ? 0 : upload_bytes;
    entry->next = surface_textures;
    surface_textures = entry;
    SDL_SetNumberProperty(properties,
                          GPU_RENDERER_SURFACE_GENERATION_PROPERTY,
                          (Sint64)entry->generation);
    statistics.upload_count++;
    statistics.upload_bytes += upload_bytes;
    if (!entry->atlased) {
        statistics.resource_creations++;
        statistics.retained_bytes += entry->bytes;
        statistics.peak_retained_bytes = MAX(statistics.peak_retained_bytes,
                                             statistics.retained_bytes);
    }
    return entry;
}

static gpu_canvas_t *gpu_renderer_canvas(SDL_Surface *surface) {
    if (surface == NULL) {
        return NULL;
    }
    return SDL_GetPointerProperty(SDL_GetSurfaceProperties(surface),
                                  GPU_RENDERER_CANVAS_PROPERTY,
                                  NULL);
}

static bool gpu_renderer_canvas_texture_create(gpu_canvas_t *canvas) {
    HARD_ASSERT(canvas != NULL);
    if (canvas->texture != NULL) {
        return true;
    }
    if (renderer == NULL) {
        return false;
    }

    canvas->texture = SDL_CreateTexture(renderer,
                                        SDL_PIXELFORMAT_RGBA8888,
                                        SDL_TEXTUREACCESS_TARGET,
                                        canvas->surface->w,
                                        canvas->surface->h);
    if (canvas->texture == NULL ||
        !SDL_SetTextureScaleMode(canvas->texture, SDL_SCALEMODE_NEAREST) ||
        !SDL_SetTextureBlendMode(canvas->texture, SDL_BLENDMODE_BLEND)) {
        gpu_renderer_canvas_texture_destroy(canvas);
        return false;
    }
    canvas->bytes = (size_t)canvas->surface->w * (size_t)canvas->surface->h * 4U;
    statistics.resource_creations++;
    statistics.retained_bytes += canvas->bytes;
    statistics.peak_retained_bytes = MAX(statistics.peak_retained_bytes,
                                         statistics.retained_bytes);

    /* A widget's initial background is immutable upload data. Subsequent
     * widget composition targets this GPU texture directly. */
    SDL_Texture *bootstrap = SDL_CreateTextureFromSurface(renderer, canvas->surface);
    SDL_Texture *previous = SDL_GetRenderTarget(renderer);
    bool success = bootstrap != NULL && SDL_SetRenderTarget(renderer, canvas->texture) &&
                   SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_TRANSPARENT) &&
                   SDL_RenderClear(renderer) &&
                   SDL_RenderTexture(renderer, bootstrap, NULL, NULL) &&
                   SDL_SetRenderTarget(renderer, previous);
    if (bootstrap != NULL) {
        SDL_DestroyTexture(bootstrap);
    }
    if (!success) {
        SDL_SetRenderTarget(renderer, previous);
        gpu_renderer_canvas_texture_destroy(canvas);
        return false;
    }
    statistics.upload_count++;
    statistics.upload_bytes += canvas->bytes;
    return true;
}

bool gpu_renderer_canvas_register(SDL_Surface *surface) {
    if (surface == NULL) {
        return false;
    }
    gpu_canvas_t *canvas = gpu_renderer_canvas(surface);
    if (canvas == NULL) {
        canvas = xcalloc(1, sizeof(*canvas));
        canvas->surface = surface;
        canvas->next = canvases;
        canvases = canvas;
        if (!SDL_SetPointerPropertyWithCleanup(SDL_GetSurfaceProperties(surface),
                                               GPU_RENDERER_CANVAS_PROPERTY,
                                               canvas,
                                               gpu_renderer_canvas_cleanup,
                                               NULL)) {
            canvases = canvas->next;
            free(canvas);
            return false;
        }
    }
    return renderer == NULL || gpu_renderer_canvas_texture_create(canvas);
}

bool gpu_renderer_canvas_registered(SDL_Surface *surface) {
    return gpu_renderer_canvas(surface) != NULL;
}

typedef struct gpu_renderer_target_scope {
    SDL_Texture *previous_target;
    SDL_Rect previous_clip;
    bool previous_clip_enabled;
} gpu_renderer_target_scope_t;

static bool gpu_renderer_target_begin(SDL_Surface *surface,
                                      gpu_renderer_target_scope_t *scope) {
    HARD_ASSERT(surface != NULL);
    HARD_ASSERT(scope != NULL);
    gpu_canvas_t *canvas = gpu_renderer_canvas(surface);
    if (canvas == NULL || !gpu_renderer_canvas_texture_create(canvas)) {
        return false;
    }
    scope->previous_target = SDL_GetRenderTarget(renderer);
    scope->previous_clip_enabled = SDL_RenderClipEnabled(renderer);
    if (scope->previous_clip_enabled) {
        SDL_GetRenderClipRect(renderer, &scope->previous_clip);
    }
    SDL_Rect clip;
    SDL_GetSurfaceClipRect(surface, &clip);
    return SDL_SetRenderTarget(renderer, canvas->texture) && SDL_SetRenderClipRect(renderer, &clip);
}

static bool gpu_renderer_target_end(const gpu_renderer_target_scope_t *scope, bool success) {
    bool restored = SDL_SetRenderTarget(renderer, scope->previous_target) &&
                    SDL_SetRenderClipRect(renderer,
                                          scope->previous_clip_enabled ? &scope->previous_clip
                                                                      : NULL);
    return success && restored;
}

static bool gpu_renderer_draw_surface_to_impl(SDL_Surface *target,
                                              SDL_Surface *surface,
                                              const SDL_Rect *source,
                                              const SDL_FRect *destination,
                                              SDL_ScaleMode scale_mode) {
    if (renderer == NULL || surface == NULL || destination == NULL) {
        return false;
    }
    if (target == NULL && gpu_map_renderer_active()) {
        return gpu_map_renderer_draw_surface(surface, source, destination);
    }
    gpu_canvas_t *source_canvas = gpu_renderer_canvas(surface);
    SDL_Texture *texture;
    gpu_surface_texture_t *surface_entry = NULL;
    if (source_canvas != NULL) {
        if (!gpu_renderer_canvas_texture_create(source_canvas)) {
            return false;
        }
        texture = source_canvas->texture;
    } else {
        surface_entry = gpu_renderer_surface_texture(surface);
        if (surface_entry == NULL) {
            return false;
        }
        texture = surface_entry->texture;
    }
    gpu_renderer_target_scope_t scope;
    if (target != NULL && !gpu_renderer_target_begin(target, &scope)) {
        return false;
    }
    Uint8 red, green, blue, alpha;
    SDL_BlendMode blend_mode;
    if (!SDL_GetSurfaceColorMod(surface, &red, &green, &blue) ||
        !SDL_GetSurfaceAlphaMod(surface, &alpha) ||
        !SDL_GetSurfaceBlendMode(surface, &blend_mode) ||
        !SDL_SetTextureScaleMode(texture, scale_mode) ||
        !SDL_SetTextureColorMod(texture, red, green, blue) ||
        !SDL_SetTextureAlphaMod(texture, alpha) ||
        !SDL_SetTextureBlendMode(texture, blend_mode)) {
        return target == NULL ? false : gpu_renderer_target_end(&scope, false);
    }
    SDL_FRect source_float;
    const SDL_FRect *source_pointer = NULL;
    if (surface_entry != NULL && surface_entry->atlased) {
        source_float = surface_entry->atlas_source;
        if (source != NULL) {
            source_float.x += (float)source->x;
            source_float.y += (float)source->y;
            source_float.w = (float)source->w;
            source_float.h = (float)source->h;
        }
        source_pointer = &source_float;
    } else if (source != NULL) {
        source_float = (SDL_FRect){(float)source->x,
                                  (float)source->y,
                                  (float)source->w,
                                  (float)source->h};
        source_pointer = &source_float;
    }
    statistics.draws++;
    bool success = SDL_RenderTexture(renderer, texture, source_pointer, destination);
    return target == NULL ? success : gpu_renderer_target_end(&scope, success);
}

bool gpu_renderer_draw_surface(SDL_Surface *surface,
                               const SDL_Rect *source,
                               const SDL_FRect *destination) {
    return gpu_renderer_frame_result(
        gpu_renderer_draw_surface_to_impl(NULL,
                                          surface,
                                          source,
                                          destination,
                                          SDL_SCALEMODE_NEAREST));
}

bool gpu_renderer_draw_surface_to(SDL_Surface *target,
                                  SDL_Surface *surface,
                                  const SDL_Rect *source,
                                  const SDL_FRect *destination) {
    return gpu_renderer_frame_result(
        gpu_renderer_draw_surface_to_impl(target,
                                          surface,
                                          source,
                                          destination,
                                          SDL_SCALEMODE_NEAREST));
}

bool gpu_renderer_draw_surface_scaled_to(SDL_Surface *target,
                                         SDL_Surface *surface,
                                         const SDL_Rect *source,
                                         const SDL_FRect *destination,
                                         SDL_ScaleMode scale_mode) {
    return gpu_renderer_frame_result(
        gpu_renderer_draw_surface_to_impl(target, surface, source, destination, scale_mode));
}

bool gpu_renderer_canvas_fill(SDL_Surface *surface,
                              const SDL_Rect *rectangle,
                              Uint8 red,
                              Uint8 green,
                              Uint8 blue,
                              Uint8 alpha) {
    if (renderer == NULL || !gpu_renderer_canvas_registered(surface)) {
        return gpu_renderer_frame_result(false);
    }
    gpu_renderer_target_scope_t scope;
    if (!gpu_renderer_target_begin(surface, &scope)) {
        return gpu_renderer_frame_result(false);
    }
    SDL_FRect destination = rectangle != NULL
                                ? (SDL_FRect){(float)rectangle->x,
                                              (float)rectangle->y,
                                              (float)rectangle->w,
                                              (float)rectangle->h}
                                : (SDL_FRect){0.0f,
                                              0.0f,
                                              (float)surface->w,
                                              (float)surface->h};
    bool success = SDL_SetRenderDrawColor(renderer, red, green, blue, alpha) &&
                   SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE) &&
                   SDL_RenderFillRect(renderer, &destination);
    statistics.draws++;
    return gpu_renderer_frame_result(gpu_renderer_target_end(&scope, success));
}

bool gpu_renderer_canvas_draw_rect(SDL_Surface *surface,
                                   const SDL_FRect *rectangle,
                                   Uint8 red,
                                   Uint8 green,
                                   Uint8 blue,
                                   Uint8 alpha,
                                   bool filled) {
    if (renderer == NULL || rectangle == NULL || !gpu_renderer_canvas_registered(surface)) {
        return gpu_renderer_frame_result(false);
    }
    gpu_renderer_target_scope_t scope;
    if (!gpu_renderer_target_begin(surface, &scope)) {
        return gpu_renderer_frame_result(false);
    }
    bool success = gpu_renderer_draw_color(red, green, blue, alpha) &&
                   (filled ? SDL_RenderFillRect(renderer, rectangle)
                           : SDL_RenderRect(renderer, rectangle));
    statistics.draws++;
    return gpu_renderer_frame_result(gpu_renderer_target_end(&scope, success));
}

bool gpu_renderer_canvas_draw_line(SDL_Surface *surface,
                                   float x1,
                                   float y1,
                                   float x2,
                                   float y2,
                                   Uint8 red,
                                   Uint8 green,
                                   Uint8 blue,
                                   Uint8 alpha) {
    if (renderer == NULL || !gpu_renderer_canvas_registered(surface)) {
        return gpu_renderer_frame_result(false);
    }
    gpu_renderer_target_scope_t scope;
    if (!gpu_renderer_target_begin(surface, &scope)) {
        return gpu_renderer_frame_result(false);
    }
    bool success = gpu_renderer_draw_color(red, green, blue, alpha) &&
                   SDL_RenderLine(renderer, x1, y1, x2, y2);
    statistics.draws++;
    return gpu_renderer_frame_result(gpu_renderer_target_end(&scope, success));
}

static bool gpu_renderer_draw_color(Uint8 red, Uint8 green, Uint8 blue, Uint8 alpha) {
    return renderer != NULL && SDL_SetRenderDrawColor(renderer, red, green, blue, alpha) &&
           SDL_SetRenderDrawBlendMode(renderer,
                                      alpha == SDL_ALPHA_OPAQUE ? SDL_BLENDMODE_NONE
                                                                : SDL_BLENDMODE_BLEND);
}

bool gpu_renderer_draw_rect(const SDL_FRect *rectangle,
                            Uint8 red,
                            Uint8 green,
                            Uint8 blue,
                            Uint8 alpha,
                            bool filled) {
    if (gpu_map_renderer_active()) {
        return gpu_map_renderer_draw_rect(rectangle, red, green, blue, alpha, filled);
    }
    if (rectangle == NULL || !gpu_renderer_draw_color(red, green, blue, alpha)) {
        return gpu_renderer_frame_result(false);
    }
    statistics.draws++;
    return gpu_renderer_frame_result(filled ? SDL_RenderFillRect(renderer, rectangle)
                                            : SDL_RenderRect(renderer, rectangle));
}

bool gpu_renderer_draw_line(float x1,
                            float y1,
                            float x2,
                            float y2,
                            Uint8 red,
                            Uint8 green,
                            Uint8 blue,
                            Uint8 alpha) {
    if (gpu_map_renderer_active()) {
        if (x1 != x2 && y1 != y2) {
            SDL_SetError("Diagonal raw GPU map lines are unsupported");
            return gpu_renderer_frame_result(false);
        }
        SDL_FRect rectangle = {
            .x = MIN(x1, x2),
            .y = MIN(y1, y2),
            .w = x1 == x2 ? 1.0f : fabsf(x2 - x1) + 1.0f,
            .h = y1 == y2 ? 1.0f : fabsf(y2 - y1) + 1.0f,
        };
        return gpu_renderer_frame_result(
            gpu_map_renderer_draw_rect(&rectangle, red, green, blue, alpha, true));
    }
    if (!gpu_renderer_draw_color(red, green, blue, alpha)) {
        return gpu_renderer_frame_result(false);
    }
    statistics.draws++;
    return gpu_renderer_frame_result(SDL_RenderLine(renderer, x1, y1, x2, y2));
}

bool gpu_renderer_set_clip(const SDL_Rect *rectangle) {
    if (gpu_map_renderer_active()) {
        return gpu_renderer_frame_result(gpu_map_renderer_set_clip(rectangle));
    }
    return gpu_renderer_frame_result(renderer != NULL && SDL_SetRenderClipRect(renderer, rectangle));
}

void gpu_renderer_invalidate_surface(SDL_Surface *surface) {
    gpu_map_renderer_invalidate_surface(surface);
    if (surface != NULL && gpu_renderer_canvas_registered(surface)) {
        SDL_ClearProperty(SDL_GetSurfaceProperties(surface), GPU_RENDERER_CANVAS_PROPERTY);
    }
    gpu_surface_texture_t **link = &surface_textures;
    while (*link != NULL) {
        gpu_surface_texture_t *entry = *link;
        if (entry->surface == surface) {
            *link = entry->next;
            gpu_renderer_surface_texture_destroy(entry);
            return;
        }
        link = &entry->next;
    }
}

void gpu_renderer_surface_changed(SDL_Surface *surface) {
    if (surface == NULL) {
        return;
    }
    gpu_renderer_invalidate_surface(surface);
    SDL_SetNumberProperty(SDL_GetSurfaceProperties(surface),
                          GPU_RENDERER_SURFACE_GENERATION_PROPERTY,
                          0);
}

SDL_Surface *gpu_renderer_readback(const SDL_Rect *rect) {
    if (renderer == NULL || device == NULL || frame_target == NULL || !SDL_FlushRenderer(renderer)) {
        return NULL;
    }
    float texture_width, texture_height;
    if (!SDL_GetTextureSize(frame_target, &texture_width, &texture_height)) {
        return NULL;
    }
    SDL_Rect area = rect != NULL ? *rect
                                 : (SDL_Rect){0, 0, (int)texture_width, (int)texture_height};
    if (area.x < 0 || area.y < 0 || area.w <= 0 || area.h <= 0 ||
        area.x + area.w > (int)texture_width || area.y + area.h > (int)texture_height) {
        SDL_SetError("GPU screenshot rectangle is outside the completed frame");
        return NULL;
    }
    SDL_GPUTexture *texture = SDL_GetPointerProperty(SDL_GetTextureProperties(frame_target),
                                                     SDL_PROP_TEXTURE_GPU_TEXTURE_POINTER,
                                                     NULL);
    if (texture == NULL) {
        SDL_SetError("GPU renderer did not expose the completed frame texture");
        return NULL;
    }

    Uint32 row_pitch = ((Uint32)area.w * 4U + 255U) & ~UINT32_C(255);
    if ((Uint64)row_pitch * (Uint64)area.h > UINT32_MAX) {
        SDL_SetError("GPU screenshot is too large");
        return NULL;
    }
    Uint32 transfer_size = row_pitch * (Uint32)area.h;
    SDL_GPUTransferBufferCreateInfo create_info = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
        .size = transfer_size,
    };
    SDL_GPUTransferBuffer *transfer = SDL_CreateGPUTransferBuffer(device, &create_info);
    SDL_GPUCommandBuffer *commands = transfer != NULL ? SDL_AcquireGPUCommandBuffer(device) : NULL;
    SDL_GPUCopyPass *copy = commands != NULL ? SDL_BeginGPUCopyPass(commands) : NULL;
    if (copy == NULL) {
        if (commands != NULL) {
            SDL_CancelGPUCommandBuffer(commands);
        }
        if (transfer != NULL) {
            SDL_ReleaseGPUTransferBuffer(device, transfer);
        }
        return NULL;
    }
    SDL_GPUTextureRegion source = {
        .texture = texture,
        .x = (Uint32)area.x,
        .y = (Uint32)area.y,
        .w = (Uint32)area.w,
        .h = (Uint32)area.h,
        .d = 1,
    };
    SDL_GPUTextureTransferInfo destination = {
        .transfer_buffer = transfer,
        .pixels_per_row = row_pitch / 4U,
        .rows_per_layer = (Uint32)area.h,
    };
    SDL_DownloadFromGPUTexture(copy, &source, &destination);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (fence == NULL || !SDL_WaitForGPUFences(device, true, &fence, 1)) {
        if (fence != NULL) {
            SDL_ReleaseGPUFence(device, fence);
        }
        SDL_ReleaseGPUTransferBuffer(device, transfer);
        return NULL;
    }

    SDL_Surface *surface = SDL_CreateSurface(area.w, area.h, SDL_PIXELFORMAT_RGBA32);
    void *pixels = SDL_MapGPUTransferBuffer(device, transfer, false);
    if (surface != NULL && pixels != NULL) {
        for (int y = 0; y < area.h; y++) {
            memcpy((Uint8 *)surface->pixels + (size_t)y * (size_t)surface->pitch,
                   (const Uint8 *)pixels + (size_t)y * row_pitch,
                   (size_t)area.w * 4U);
        }
    } else {
        SDL_DestroySurface(surface);
        surface = NULL;
    }
    if (pixels != NULL) {
        SDL_UnmapGPUTransferBuffer(device, transfer);
    }
    SDL_ReleaseGPUFence(device, fence);
    SDL_ReleaseGPUTransferBuffer(device, transfer);
    return surface;
}

void gpu_renderer_statistics_reset(void) {
    uint64_t retained_bytes = statistics.retained_bytes;
    memset(&statistics, 0, sizeof(statistics));
    statistics.retained_bytes = retained_bytes;
    statistics.peak_retained_bytes = retained_bytes;
}

void gpu_renderer_statistics_get(gpu_renderer_statistics_t *result) {
    HARD_ASSERT(result != NULL);
    *result = statistics;
}

uint64_t gpu_renderer_timing_begin(void) {
    return SDL_GetTicksNS();
}

void gpu_renderer_timing_end(gpu_renderer_timing_stage_t stage, uint64_t started_ns) {
    HARD_ASSERT(stage >= 0 && stage < GPU_RENDERER_TIMING_NUM);
    if (started_ns == 0) {
        return;
    }
    statistics.timings[stage].calls++;
    statistics.timings[stage].elapsed_ns += SDL_GetTicksNS() - started_ns;
}

void gpu_renderer_statistics_commands(uint64_t commands, uint64_t batches, uint64_t draws) {
    statistics.commands += commands;
    statistics.batches += batches;
    statistics.draws += draws;
}

void gpu_renderer_statistics_upload(size_t bytes) {
    statistics.upload_count++;
    statistics.upload_bytes += bytes;
}

void gpu_renderer_statistics_resource_create(size_t retained_bytes) {
    statistics.resource_creations++;
    statistics.retained_bytes += retained_bytes;
    statistics.peak_retained_bytes = MAX(statistics.peak_retained_bytes,
                                         statistics.retained_bytes);
}

void gpu_renderer_statistics_resource_destroy(size_t retained_bytes) {
    statistics.resource_destructions++;
    HARD_ASSERT(statistics.retained_bytes >= retained_bytes);
    statistics.retained_bytes -= retained_bytes;
}

void gpu_renderer_statistics_recovery(bool succeeded) {
    statistics.device_recoveries++;
    statistics.recovery_failures += !succeeded;
}
