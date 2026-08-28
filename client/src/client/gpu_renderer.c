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

typedef struct gpu_surface_texture {
    SDL_Surface *surface;
    SDL_Texture *texture;
    Uint64 generation;
    size_t bytes;
    struct gpu_surface_texture *next;
} gpu_surface_texture_t;

static SDL_Renderer *renderer;
static SDL_GPUDevice *device;
static gpu_surface_texture_t *surface_textures;
static Uint64 next_surface_generation = 1;
static gpu_renderer_statistics_t statistics;
static char backend[32];
static char device_name[256];
static char driver_name[256];
static char driver_version[256];

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
    if (entry->texture != NULL) {
        SDL_DestroyTexture(entry->texture);
        statistics.resource_destructions++;
        statistics.retained_bytes -= entry->bytes;
    }
    free(entry);
}

static void gpu_renderer_surface_textures_destroy(void) {
    while (surface_textures != NULL) {
        gpu_surface_texture_t *entry = surface_textures;
        surface_textures = entry->next;
        gpu_renderer_surface_texture_destroy(entry);
    }
}

static void gpu_renderer_device_destroy(void) {
    gpu_renderer_surface_textures_destroy();
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
    SDL_SetDefaultTextureScaleMode(renderer, SDL_SCALEMODE_NEAREST);

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

bool gpu_renderer_begin_frame(void) {
    if (renderer == NULL || !SDL_SetRenderTarget(renderer, NULL) ||
        !SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE)) {
        return false;
    }
    return SDL_RenderClear(renderer);
}

bool gpu_renderer_present(void) {
    if (renderer == NULL) {
        return false;
    }
    uint64_t started = gpu_renderer_timing_begin();
    bool result = SDL_RenderPresent(renderer);
    gpu_renderer_timing_end(GPU_RENDERER_TIMING_PRESENT_WAIT, started);
    return result;
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
    entry->texture = SDL_CreateTextureFromSurface(renderer, surface);
    if (entry->texture == NULL) {
        free(entry);
        return NULL;
    }
    SDL_SetTextureScaleMode(entry->texture, SDL_SCALEMODE_NEAREST);
    entry->surface = surface;
    entry->generation = next_surface_generation++;
    if (next_surface_generation == 0) {
        next_surface_generation = 1;
    }
    entry->bytes = (size_t)surface->w * (size_t)surface->h * 4U;
    entry->next = surface_textures;
    surface_textures = entry;
    SDL_SetNumberProperty(properties,
                          GPU_RENDERER_SURFACE_GENERATION_PROPERTY,
                          (Sint64)entry->generation);
    statistics.upload_count++;
    statistics.upload_bytes += entry->bytes;
    statistics.resource_creations++;
    statistics.retained_bytes += entry->bytes;
    statistics.peak_retained_bytes = MAX(statistics.peak_retained_bytes,
                                         statistics.retained_bytes);
    return entry;
}

bool gpu_renderer_draw_surface(SDL_Surface *surface,
                               const SDL_Rect *source,
                               const SDL_FRect *destination) {
    if (renderer == NULL || surface == NULL || destination == NULL) {
        return false;
    }
    gpu_surface_texture_t *entry = gpu_renderer_surface_texture(surface);
    if (entry == NULL) {
        return false;
    }
    Uint8 red, green, blue, alpha;
    SDL_BlendMode blend_mode;
    if (!SDL_GetSurfaceColorMod(surface, &red, &green, &blue) ||
        !SDL_GetSurfaceAlphaMod(surface, &alpha) ||
        !SDL_GetSurfaceBlendMode(surface, &blend_mode) ||
        !SDL_SetTextureColorMod(entry->texture, red, green, blue) ||
        !SDL_SetTextureAlphaMod(entry->texture, alpha) ||
        !SDL_SetTextureBlendMode(entry->texture, blend_mode)) {
        return false;
    }
    SDL_FRect source_float;
    const SDL_FRect *source_pointer = NULL;
    if (source != NULL) {
        source_float = (SDL_FRect){(float)source->x,
                                  (float)source->y,
                                  (float)source->w,
                                  (float)source->h};
        source_pointer = &source_float;
    }
    statistics.draws++;
    return SDL_RenderTexture(renderer, entry->texture, source_pointer, destination);
}

void gpu_renderer_invalidate_surface(SDL_Surface *surface) {
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

SDL_Surface *gpu_renderer_readback(const SDL_Rect *rect) {
    return renderer != NULL ? SDL_RenderReadPixels(renderer, rect) : NULL;
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

void gpu_renderer_statistics_recovery(bool succeeded) {
    statistics.device_recoveries++;
    statistics.recovery_failures += !succeeded;
}
