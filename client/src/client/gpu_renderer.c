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

#ifdef WIN32
#define COBJMACROS
#include <winsock2.h>
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#endif

#include <global.h>

#define GPU_RENDERER_SURFACE_GENERATION_PROPERTY "atrinik.gpu.surface_generation"
#define GPU_RENDERER_SURFACE_TEXTURE_PROPERTY "atrinik.gpu.surface_texture"
#define GPU_RENDERER_CANVAS_PROPERTY "atrinik.gpu.canvas"
#define GPU_RENDERER_ATLAS_SIZE 2048
#define GPU_RENDERER_ATLAS_ENTRY_LIMIT 512

typedef struct gpu_texture_atlas gpu_texture_atlas_t;

typedef struct gpu_surface_texture {
    SDL_Surface *surface;
    SDL_Texture *texture;
    gpu_texture_atlas_t *atlas;
    Uint64 generation;
    size_t bytes;
    SDL_FRect atlas_source;
    bool atlased;
    bool accounted;
    struct gpu_surface_texture *next;
} gpu_surface_texture_t;

typedef struct gpu_atlas_region {
    SDL_Rect rectangle;
    struct gpu_atlas_region *next;
} gpu_atlas_region_t;

struct gpu_texture_atlas {
    SDL_Texture *texture;
    gpu_atlas_region_t *free_regions;
    size_t allocations;
    size_t bytes;
    gpu_texture_atlas_t *next;
};

typedef struct gpu_canvas {
    SDL_Surface *surface;
    SDL_Texture *texture;
    size_t bytes;
    bool accounted;
    struct gpu_canvas *next;
} gpu_canvas_t;

typedef struct gpu_pending_readback {
    SDL_GPUTransferBuffer *transfer;
    SDL_GPUFence *fence;
    SDL_Rect area;
    Uint32 row_pitch;
    gpu_renderer_readback_callback_t callback;
    gpu_renderer_readback_cancel_callback_t cancel_callback;
    void *userdata;
    struct gpu_pending_readback *next;
} gpu_pending_readback_t;

static SDL_Renderer *renderer;
static SDL_GPUDevice *device;
static SDL_Texture *frame_target;
static size_t frame_target_bytes;
static gpu_surface_texture_t *surface_textures;
static gpu_texture_atlas_t *texture_atlases;
static gpu_canvas_t *canvases;
static gpu_pending_readback_t *pending_readbacks;
static Uint64 next_surface_generation = 1;
static gpu_renderer_statistics_t statistics;
#ifdef ATRINIK_GPU_CONFORMANCE_TESTS
static gpu_renderer_conformance_fault_t conformance_fault;
#endif
static char backend[32];
static char device_name[256];
static char driver_name[256];
static char driver_version[256];
static bool frame_failed;
static bool recreation_requested;
static bool hardware_verified;

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

#ifdef WIN32
static bool gpu_renderer_qualification_requested(void) {
    const char *value = SDL_GetEnvironmentVariable(SDL_GetEnvironment(),
                                                   "ATRINIK_GPU_CONFORMANCE_QUALIFIED_HARDWARE");
    return value != NULL && strcmp(value, "1") == 0;
}
#endif

static bool gpu_renderer_d3d12_hardware_verified(void) {
#ifdef WIN32
    IDXGIFactory1 *factory1 = NULL;
    HRESULT result = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&factory1);
    if (FAILED(result)) {
        SDL_SetError("could not create DXGI factory for D3D12 hardware attestation");
        return false;
    }
    unsigned int hardware_adapters = 0;
    for (UINT index = 0;; index++) {
        IDXGIAdapter1 *adapter = NULL;
        result = IDXGIFactory1_EnumAdapters1(factory1, index, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(result) || adapter == NULL) {
            IDXGIFactory1_Release(factory1);
            SDL_SetError("could not enumerate DXGI adapters for D3D12 hardware attestation");
            return false;
        }
        DXGI_ADAPTER_DESC1 description;
        ID3D12Device *candidate_device = NULL;
        bool hardware = SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &description)) &&
                        !(description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) &&
                        SUCCEEDED(D3D12CreateDevice((IUnknown *)adapter,
                                                    D3D_FEATURE_LEVEL_11_0,
                                                    &IID_ID3D12Device,
                                                    (void **)&candidate_device));
        if (candidate_device != NULL) {
            ID3D12Device_Release(candidate_device);
        }
        IDXGIAdapter1_Release(adapter);
        hardware_adapters += hardware;
    }
    IDXGIFactory1_Release(factory1);
    bool unique_required = gpu_renderer_qualification_requested();
    if (hardware_adapters == 0U || (unique_required && hardware_adapters != 1U)) {
        SDL_SetError("D3D12 hardware attestation requires %s hardware-capable DXGI adapter "
                     "(found %u)",
                     unique_required ? "exactly one" : "at least one",
                     hardware_adapters);
        return false;
    }
    /* SDL 3.4 does not expose the selected ID3D12Device/LUID. Qualification
     * runners are constrained to one D3D12-capable non-software adapter so
     * SDL's successful selection is unambiguous. Normal production accepts
     * hybrid/multi-GPU systems when at least one hardware adapter qualifies. */
    return true;
#else
    SDL_SetError("D3D12 hardware attestation is unavailable on this platform");
    return false;
#endif
}

static bool gpu_renderer_hardware_attest(const char *name, bool require_hardware) {
    if (!require_hardware) {
        return false;
    }
    if (strcmp(name, "vulkan") == 0 || strcmp(name, "metal") == 0) {
        return true;
    }
    return strcmp(name, "direct3d12") == 0 && gpu_renderer_d3d12_hardware_verified();
}

static bool gpu_renderer_formats_supported(SDL_GPUDevice *candidate) {
    const SDL_GPUTextureUsageFlags sampled_usage =
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    const SDL_GPUTextureUsageFlags integer_usage =
        SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_GRAPHICS_STORAGE_READ;
    return SDL_GPUTextureSupportsFormat(candidate,
                                        SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                                        SDL_GPU_TEXTURETYPE_2D,
                                        sampled_usage) &&
           SDL_GPUTextureSupportsFormat(candidate,
                                        SDL_GPU_TEXTUREFORMAT_R32_UINT,
                                        SDL_GPU_TEXTURETYPE_2D,
                                        integer_usage);
}

static void gpu_renderer_atlas_regions_destroy(gpu_texture_atlas_t *atlas) {
    while (atlas->free_regions != NULL) {
        gpu_atlas_region_t *region = atlas->free_regions;
        atlas->free_regions = region->next;
        free(region);
    }
}

static void gpu_renderer_texture_atlas_destroy(gpu_texture_atlas_t *atlas) {
    SDL_DestroyTexture(atlas->texture);
    gpu_renderer_atlas_regions_destroy(atlas);
    statistics.resource_destructions++;
    statistics.retained_bytes -= atlas->bytes;
    free(atlas);
}

static void gpu_renderer_texture_atlas_reset(gpu_texture_atlas_t *atlas) {
    HARD_ASSERT(atlas->allocations == 0);
    gpu_renderer_atlas_regions_destroy(atlas);
    atlas->free_regions = xcalloc(1, sizeof(*atlas->free_regions));
    atlas->free_regions->rectangle =
        (SDL_Rect){0, 0, GPU_RENDERER_ATLAS_SIZE, GPU_RENDERER_ATLAS_SIZE};
}

/** Keep one pristine empty page warm and reclaim every additional empty page. */
static void gpu_renderer_texture_atlas_empty_pages_trim(void) {
    bool warm_page_retained = false;
    gpu_texture_atlas_t **link = &texture_atlases;
    while (*link != NULL) {
        gpu_texture_atlas_t *atlas = *link;
        if (atlas->allocations != 0) {
            link = &atlas->next;
            continue;
        }
        if (!warm_page_retained) {
            gpu_renderer_texture_atlas_reset(atlas);
            warm_page_retained = true;
            link = &atlas->next;
            continue;
        }
        *link = atlas->next;
        atlas->next = NULL;
        gpu_renderer_texture_atlas_destroy(atlas);
    }
}

static void gpu_renderer_atlas_region_release(gpu_texture_atlas_t *atlas, SDL_Rect rectangle) {
    HARD_ASSERT(atlas->allocations != 0);
    gpu_atlas_region_t *region = xcalloc(1, sizeof(*region));
    region->rectangle = rectangle;
    bool merged;
    do {
        merged = false;
        for (gpu_atlas_region_t **link = &atlas->free_regions; *link != NULL;
             link = &(*link)->next) {
            gpu_atlas_region_t *other = *link;
            if (region->rectangle.y == other->rectangle.y &&
                region->rectangle.h == other->rectangle.h &&
                (region->rectangle.x + region->rectangle.w == other->rectangle.x ||
                 other->rectangle.x + other->rectangle.w == region->rectangle.x)) {
                int left = MIN(region->rectangle.x, other->rectangle.x);
                int right = MAX(region->rectangle.x + region->rectangle.w,
                                other->rectangle.x + other->rectangle.w);
                region->rectangle.x = left;
                region->rectangle.w = right - left;
            } else if (region->rectangle.x == other->rectangle.x &&
                       region->rectangle.w == other->rectangle.w &&
                       (region->rectangle.y + region->rectangle.h == other->rectangle.y ||
                        other->rectangle.y + other->rectangle.h == region->rectangle.y)) {
                int top = MIN(region->rectangle.y, other->rectangle.y);
                int bottom = MAX(region->rectangle.y + region->rectangle.h,
                                 other->rectangle.y + other->rectangle.h);
                region->rectangle.y = top;
                region->rectangle.h = bottom - top;
            } else {
                continue;
            }
            *link = other->next;
            free(other);
            merged = true;
            break;
        }
    } while (merged);
    region->next = atlas->free_regions;
    atlas->free_regions = region;
    atlas->allocations--;
    gpu_renderer_texture_atlas_empty_pages_trim();
}

static void gpu_renderer_surface_texture_destroy(gpu_surface_texture_t *entry) {
    if (entry->texture != NULL && !entry->atlased) {
        SDL_DestroyTexture(entry->texture);
        if (entry->accounted) {
            statistics.resource_destructions++;
            statistics.retained_bytes -= entry->bytes;
        }
    } else if (entry->atlased && entry->atlas != NULL) {
        gpu_renderer_atlas_region_release(entry->atlas,
                                          (SDL_Rect){(int)entry->atlas_source.x,
                                                     (int)entry->atlas_source.y,
                                                     (int)entry->atlas_source.w,
                                                     (int)entry->atlas_source.h});
    }
    free(entry);
}

static void SDLCALL gpu_renderer_surface_texture_cleanup(void *userdata, void *value) {
    (void)userdata;
    gpu_surface_texture_t *entry = value;
    gpu_surface_texture_t **link = &surface_textures;
    while (*link != NULL && *link != entry) {
        link = &(*link)->next;
    }
    if (*link == entry) {
        *link = entry->next;
    }
    gpu_renderer_surface_texture_destroy(entry);
}

static void gpu_renderer_texture_atlases_destroy(void) {
    while (texture_atlases != NULL) {
        gpu_texture_atlas_t *atlas = texture_atlases;
        texture_atlases = atlas->next;
        HARD_ASSERT(atlas->allocations == 0);
        atlas->next = NULL;
        gpu_renderer_texture_atlas_destroy(atlas);
    }
}

static void gpu_renderer_surface_textures_destroy(void) {
    while (surface_textures != NULL) {
        gpu_surface_texture_t *entry = surface_textures;
        SDL_PropertiesID properties = SDL_GetSurfaceProperties(entry->surface);
        if (SDL_GetPointerProperty(properties, GPU_RENDERER_SURFACE_TEXTURE_PROPERTY, NULL) ==
            entry) {
            SDL_ClearProperty(properties, GPU_RENDERER_SURFACE_TEXTURE_PROPERTY);
        } else {
            surface_textures = entry->next;
            gpu_renderer_surface_texture_destroy(entry);
        }
    }
}

static void gpu_renderer_canvas_texture_destroy(gpu_canvas_t *canvas) {
    if (canvas->texture == NULL) {
        return;
    }
    SDL_DestroyTexture(canvas->texture);
    canvas->texture = NULL;
    if (canvas->accounted) {
        statistics.resource_destructions++;
        statistics.retained_bytes -= canvas->bytes;
        canvas->accounted = false;
    }
    canvas->bytes = 0;
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

static void gpu_renderer_readbacks_cancel(void) {
    while (pending_readbacks != NULL) {
        gpu_pending_readback_t *pending = pending_readbacks;
        pending_readbacks = pending->next;
        if (pending->fence != NULL) {
            SDL_ReleaseGPUFence(device, pending->fence);
        }
        if (pending->transfer != NULL) {
            SDL_ReleaseGPUTransferBuffer(device, pending->transfer);
        }
        if (pending->cancel_callback != NULL) {
            pending->cancel_callback(pending->userdata);
        }
        free(pending);
    }
}

static void gpu_renderer_device_destroy(void) {
    if (device != NULL) {
        gpu_renderer_readbacks_cancel();
    }
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
}

static void gpu_renderer_identity_clear(void) {
    backend[0] = '\0';
    device_name[0] = '\0';
    driver_name[0] = '\0';
    driver_version[0] = '\0';
    hardware_verified = false;
}

static void gpu_renderer_failure_preserve(const char *context) {
    char error[512];
    snprintf(error, sizeof(error), "%s", SDL_GetError());
    gpu_renderer_device_destroy();
    SDL_SetError("%s (backend: %s; device: %s; driver: %s %s): %s",
                 context,
                 backend[0] != '\0' ? backend : "unavailable",
                 device_name[0] != '\0' ? device_name : "unavailable",
                 driver_name[0] != '\0' ? driver_name : "unavailable",
                 driver_version[0] != '\0' ? driver_version : "unavailable",
                 error[0] != '\0' ? error : "unspecified failure");
}

static bool gpu_renderer_create_internal(SDL_Window *window, bool require_hardware) {
    HARD_ASSERT(window != NULL);
    gpu_renderer_device_destroy();

    SDL_PropertiesID properties = SDL_CreateProperties();
    if (properties == 0) {
        gpu_renderer_failure_preserve("unable to create GPU device properties");
        return false;
    }
    bool configured =
        SDL_SetBooleanProperty(properties,
                               SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN,
                               true) &&
        SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXIL_BOOLEAN, true) &&
        SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN, true) &&
        SDL_SetBooleanProperty(
            properties,
            SDL_PROP_GPU_DEVICE_CREATE_VULKAN_REQUIRE_HARDWARE_ACCELERATION_BOOLEAN,
            require_hardware);
#ifdef ATRINIK_GPU_CONFORMANCE_TESTS
    if (configured) {
        if (!require_hardware) {
            configured = SDL_SetBooleanProperty(properties,
                                                SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN,
                                                true);
        }
        const char *conformance_driver =
            SDL_GetEnvironmentVariable(SDL_GetEnvironment(), "ATRINIK_GPU_CONFORMANCE_DRIVER");
        if (configured && conformance_driver != NULL && *conformance_driver != '\0') {
            configured = SDL_SetStringProperty(properties,
                                               SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING,
                                               conformance_driver);
        }
    }
#endif
    if (configured) {
        device = SDL_CreateGPUDeviceWithProperties(properties);
    }
    SDL_DestroyProperties(properties);
    if (!configured) {
        gpu_renderer_failure_preserve("unable to configure the hardware GPU device");
        return false;
    }
    if (device == NULL) {
        gpu_renderer_failure_preserve("unable to create the hardware GPU device");
        return false;
    }

    const char *selected_backend = SDL_GetGPUDeviceDriver(device);
    snprintf(backend,
             sizeof(backend),
             "%s",
             selected_backend != NULL ? selected_backend : "unavailable");
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
    if (!gpu_renderer_backend_supported(selected_backend) ||
        !gpu_renderer_formats_supported(device) ||
        (require_hardware &&
         !(hardware_verified = gpu_renderer_hardware_attest(selected_backend, true)))) {
        SDL_SetError("GPU renderer requires Vulkan, Direct3D 12, or Metal with "
                     "verified hardware acceleration plus R8G8B8A8_UNORM and R32_UINT "
                     "render targets");
        gpu_renderer_failure_preserve("unsupported GPU renderer capabilities");
        return false;
    }

    renderer = SDL_CreateGPURenderer(device, window);
    if (renderer == NULL || SDL_GetGPURendererDevice(renderer) != device) {
        gpu_renderer_failure_preserve("unable to create the SDL GPU renderer");
        return false;
    }
    if (!SDL_SetDefaultTextureScaleMode(renderer, SDL_SCALEMODE_NEAREST) ||
        !gpu_map_renderer_create(device, renderer)) {
        gpu_renderer_failure_preserve("unable to create the GPU map pipeline");
        return false;
    }
    return true;
}

bool gpu_renderer_create(SDL_Window *window) {
    return gpu_renderer_create_internal(window, true);
}

bool gpu_renderer_recover(SDL_Window *window) {
    bool succeeded = gpu_renderer_create(window);
    gpu_renderer_statistics_recovery(succeeded);
    return succeeded;
}

bool gpu_renderer_recover_bounded(SDL_Window *window,
                                  unsigned int *attempts,
                                  bool conformance_device) {
    HARD_ASSERT(window != NULL);
    HARD_ASSERT(attempts != NULL);
    if (*attempts >= 1U) {
        SDL_SetError("GPU renderer recovery retry limit reached");
        return false;
    }
    (*attempts)++;
#ifdef ATRINIK_GPU_CONFORMANCE_TESTS
    if (conformance_device) {
        return gpu_renderer_create_internal(window, false);
    }
#else
    if (conformance_device) {
        SDL_SetError("CPU-emulated GPU recovery is unavailable in production builds");
        return false;
    }
#endif
    return gpu_renderer_create_internal(window, true);
}

bool gpu_renderer_recover_and_republish(SDL_Window *window,
                                        unsigned int *attempts,
                                        bool conformance_device,
                                        gpu_renderer_recovery_step_fn apply_window_state,
                                        gpu_renderer_recovery_step_fn republish_complete_frame,
                                        void *userdata) {
    HARD_ASSERT(republish_complete_frame != NULL);
    (void)gpu_renderer_recreation_take_request();
    unsigned int attempts_before = *attempts;
    bool succeeded = gpu_renderer_recover_bounded(window, attempts, conformance_device) &&
                     (apply_window_state == NULL || apply_window_state(userdata)) &&
                     republish_complete_frame(userdata);
    if (*attempts != attempts_before) {
        gpu_renderer_statistics_recovery(succeeded);
    }
    if (!succeeded) {
        /* Never leave a recreated device available with an incomplete scene.
         * The caller may make a new, separately bounded recovery decision. */
        gpu_renderer_destroy();
        return false;
    }
    return true;
}

#ifdef ATRINIK_GPU_CONFORMANCE_TESTS
void gpu_renderer_conformance_fault_set(gpu_renderer_conformance_fault_t fault) {
    conformance_fault = fault;
}

bool gpu_renderer_conformance_fault_take(gpu_renderer_conformance_fault_t fault) {
    if (conformance_fault != fault) {
        return false;
    }
    conformance_fault = GPU_RENDERER_CONFORMANCE_FAULT_NONE;
    SDL_SetError("injected GPU conformance fault %d", (int)fault);
    return true;
}

bool gpu_renderer_conformance_available(void) {
    SDL_PropertiesID properties = SDL_CreateProperties();
    if (properties == 0) {
        return false;
    }
    bool configured =
        SDL_SetBooleanProperty(properties,
                               SDL_PROP_GPU_DEVICE_CREATE_SHADERS_SPIRV_BOOLEAN,
                               true) &&
        SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXIL_BOOLEAN, true) &&
        SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN, true) &&
        SDL_SetBooleanProperty(
            properties,
            SDL_PROP_GPU_DEVICE_CREATE_VULKAN_REQUIRE_HARDWARE_ACCELERATION_BOOLEAN,
            false) &&
        SDL_SetBooleanProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, true);
    const char *driver =
        SDL_GetEnvironmentVariable(SDL_GetEnvironment(), "ATRINIK_GPU_CONFORMANCE_DRIVER");
    if (configured && driver != NULL && *driver != '\0') {
        configured =
            SDL_SetStringProperty(properties, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, driver);
    }
    SDL_GPUDevice *candidate = configured ? SDL_CreateGPUDeviceWithProperties(properties) : NULL;
    SDL_DestroyProperties(properties);
    if (candidate == NULL) {
        return false;
    }
    bool available = gpu_renderer_backend_supported(SDL_GetGPUDeviceDriver(candidate)) &&
                     gpu_renderer_formats_supported(candidate);
    SDL_DestroyGPUDevice(candidate);
    return available;
}

bool gpu_renderer_create_conformance(SDL_Window *window) {
    return gpu_renderer_create_internal(window, false);
}

bool gpu_renderer_recover_conformance(SDL_Window *window) {
    bool succeeded = gpu_renderer_create_internal(window, false);
    gpu_renderer_statistics_recovery(succeeded);
    return succeeded;
}

bool gpu_renderer_conformance_wait_idle(void) {
    return gpu_renderer_wait_idle();
}

size_t gpu_renderer_atlas_page_count(void) {
    size_t count = 0;
    for (gpu_texture_atlas_t *atlas = texture_atlases; atlas != NULL; atlas = atlas->next) {
        count++;
    }
    return count;
}

size_t gpu_renderer_atlas_allocation_count(void) {
    size_t count = 0;
    for (gpu_texture_atlas_t *atlas = texture_atlases; atlas != NULL; atlas = atlas->next) {
        count += atlas->allocations;
    }
    return count;
}
#endif

void gpu_renderer_recreation_request(void) {
    recreation_requested = true;
}

bool gpu_renderer_recreation_take_request(void) {
    bool requested = recreation_requested;
    recreation_requested = false;
    return requested;
}

bool gpu_renderer_wait_idle(void) {
    if (renderer == NULL || device == NULL || !SDL_FlushRenderer(renderer)) {
        return false;
    }
    uint64_t started = gpu_renderer_timing_begin();
    bool succeeded = SDL_WaitForGPUIdle(device);
    gpu_renderer_timing_end(GPU_RENDERER_TIMING_COMPLETION, started);
    return succeeded;
}

void gpu_renderer_destroy(void) {
    gpu_renderer_device_destroy();
    gpu_renderer_identity_clear();
}

bool gpu_renderer_ready(void) {
    return renderer != NULL && device != NULL;
}

bool gpu_renderer_hardware_verified(void) {
    return device != NULL && hardware_verified;
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
#ifdef ATRINIK_GPU_CONFORMANCE_TESTS
    if (gpu_renderer_conformance_fault_take(GPU_RENDERER_CONFORMANCE_FAULT_ALLOCATION)) {
        return false;
    }
#endif
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
    statistics.peak_retained_bytes = MAX(statistics.peak_retained_bytes, statistics.retained_bytes);
    return true;
}

bool gpu_renderer_begin_frame(void) {
    if (recreation_requested) {
        SDL_SetError("GPU resource recovery is pending");
        return gpu_renderer_frame_result(false);
    }
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
#ifdef ATRINIK_GPU_CONFORMANCE_TESTS
    if (gpu_renderer_conformance_fault_take(GPU_RENDERER_CONFORMANCE_FAULT_SWAPCHAIN) ||
        gpu_renderer_conformance_fault_take(GPU_RENDERER_CONFORMANCE_FAULT_DEVICE_LOSS)) {
        return gpu_renderer_frame_result(false);
    }
#endif
    uint64_t started = gpu_renderer_timing_begin();
    bool result =
        SDL_SetRenderTarget(renderer, NULL) &&
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_OPAQUE) &&
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE) && SDL_RenderClear(renderer) &&
        SDL_RenderTexture(renderer, frame_target, NULL, NULL) && SDL_RenderPresent(renderer);
    statistics.draws++;
    gpu_renderer_timing_end(GPU_RENDERER_TIMING_PRESENT_WAIT, started);
    return gpu_renderer_frame_result(result);
}

bool gpu_renderer_frame_valid(void) {
    return renderer != NULL && !frame_failed;
}

bool gpu_renderer_map_begin(int width, int height) {
    return gpu_renderer_frame_result(gpu_map_renderer_begin(width, height, false));
}

bool gpu_renderer_map_begin_auxiliary(int width, int height) {
    return gpu_renderer_frame_result(gpu_map_renderer_begin(width, height, true));
}

void gpu_renderer_map_set_owner(uint8_t owner, int sample_y) {
    gpu_map_renderer_set_owner(owner, sample_y);
}

void gpu_renderer_map_set_instance_identity(uint64_t record_identity, uint32_t draw_variant) {
    gpu_map_renderer_set_instance_identity(record_identity, draw_variant);
}

void gpu_renderer_map_light_quad(uint8_t owner, const lighting_vertex_t vertices[4]) {
    gpu_map_renderer_light_quad(owner, vertices);
}

bool gpu_renderer_map_end(void) {
    return gpu_renderer_frame_result(gpu_map_renderer_end());
}

bool gpu_renderer_draw_map(float x, float y, float width, float height) {
    SDL_Texture *map_target = gpu_map_renderer_texture(false);
    if (renderer == NULL || map_target == NULL || width <= 0.0f || height <= 0.0f) {
        return false;
    }
    SDL_FRect destination = {x, y, width, height};
    statistics.draws++;
    return gpu_renderer_frame_result(
        SDL_SetTextureScaleMode(
            map_target,
            zoom_filter_to_scale_mode(setting_get_int(OPT_CAT_CLIENT, OPT_ZOOM_FILTER))) &&
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
    bool success =
        atlas->texture != NULL && SDL_SetTextureScaleMode(atlas->texture, SDL_SCALEMODE_NEAREST) &&
        SDL_SetTextureBlendMode(atlas->texture, SDL_BLENDMODE_BLEND) &&
        SDL_SetRenderTarget(renderer, atlas->texture) && SDL_SetRenderClipRect(renderer, NULL) &&
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_TRANSPARENT) &&
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE) && SDL_RenderClear(renderer) &&
        SDL_SetRenderTarget(renderer, previous) &&
        SDL_SetRenderClipRect(renderer, previous_clip_enabled ? &previous_clip : NULL);
    if (!success) {
        SDL_SetRenderTarget(renderer, previous);
        SDL_SetRenderClipRect(renderer, previous_clip_enabled ? &previous_clip : NULL);
        SDL_DestroyTexture(atlas->texture);
        free(atlas);
        return NULL;
    }
    atlas->free_regions = xcalloc(1, sizeof(*atlas->free_regions));
    atlas->free_regions->rectangle =
        (SDL_Rect){0, 0, GPU_RENDERER_ATLAS_SIZE, GPU_RENDERER_ATLAS_SIZE};
    atlas->bytes = (size_t)GPU_RENDERER_ATLAS_SIZE * GPU_RENDERER_ATLAS_SIZE * 4U;
    atlas->next = texture_atlases;
    texture_atlases = atlas;
    statistics.resource_creations++;
    statistics.retained_bytes += atlas->bytes;
    statistics.peak_retained_bytes = MAX(statistics.peak_retained_bytes, statistics.retained_bytes);
    return atlas;
}

static bool gpu_renderer_texture_atlas_place(SDL_Surface *surface,
                                             SDL_Texture **texture,
                                             SDL_FRect *source,
                                             gpu_texture_atlas_t **selected_atlas,
                                             bool *attempted) {
    HARD_ASSERT(surface != NULL);
    HARD_ASSERT(texture != NULL);
    HARD_ASSERT(source != NULL);
    HARD_ASSERT(selected_atlas != NULL);
    HARD_ASSERT(attempted != NULL);
    *attempted = false;
    if (surface->w <= 0 || surface->h <= 0 || surface->w > GPU_RENDERER_ATLAS_ENTRY_LIMIT ||
        surface->h > GPU_RENDERER_ATLAS_ENTRY_LIMIT) {
        return false;
    }
    *attempted = true;

    gpu_texture_atlas_t *atlas = texture_atlases;
    gpu_atlas_region_t **available = NULL;
    for (;;) {
        if (atlas == NULL) {
            atlas = gpu_renderer_texture_atlas_create();
            if (atlas == NULL) {
                return false;
            }
        }
        for (gpu_atlas_region_t **link = &atlas->free_regions; *link != NULL;
             link = &(*link)->next) {
            if ((*link)->rectangle.w >= surface->w && (*link)->rectangle.h >= surface->h) {
                available = link;
                break;
            }
        }
        if (available != NULL) {
            break;
        }
        atlas = atlas->next;
    }

    gpu_atlas_region_t *region = *available;
    SDL_Rect allocation = {region->rectangle.x, region->rectangle.y, surface->w, surface->h};
    *available = region->next;
    if (region->rectangle.w > surface->w) {
        gpu_atlas_region_t *right = xcalloc(1, sizeof(*right));
        right->rectangle = (SDL_Rect){region->rectangle.x + surface->w,
                                      region->rectangle.y,
                                      region->rectangle.w - surface->w,
                                      surface->h};
        right->next = atlas->free_regions;
        atlas->free_regions = right;
    }
    if (region->rectangle.h > surface->h) {
        gpu_atlas_region_t *below = xcalloc(1, sizeof(*below));
        below->rectangle = (SDL_Rect){region->rectangle.x,
                                      region->rectangle.y + surface->h,
                                      region->rectangle.w,
                                      region->rectangle.h - surface->h};
        below->next = atlas->free_regions;
        atlas->free_regions = below;
    }
    free(region);
    atlas->allocations++;

#ifdef ATRINIK_GPU_CONFORMANCE_TESTS
    if (gpu_renderer_conformance_fault_take(GPU_RENDERER_CONFORMANCE_FAULT_UI_ATLAS_UPLOAD)) {
        gpu_renderer_atlas_region_release(atlas, allocation);
        return false;
    }
#endif

    SDL_Texture *upload = SDL_CreateTextureFromSurface(renderer, surface);
    if (upload == NULL) {
        gpu_renderer_atlas_region_release(atlas, allocation);
        return false;
    }
    SDL_FRect destination = {(float)allocation.x,
                             (float)allocation.y,
                             (float)allocation.w,
                             (float)allocation.h};
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
                   SDL_SetRenderClipRect(renderer, previous_clip_enabled ? &previous_clip : NULL);
    SDL_DestroyTexture(upload);
    if (!success) {
        SDL_SetRenderTarget(renderer, previous);
        SDL_SetRenderClipRect(renderer, previous_clip_enabled ? &previous_clip : NULL);
        gpu_renderer_atlas_region_release(atlas, allocation);
        return false;
    }

    *texture = atlas->texture;
    *source = destination;
    *selected_atlas = atlas;
    return true;
}

static gpu_surface_texture_t *gpu_renderer_surface_texture(SDL_Surface *surface) {
    SDL_PropertiesID properties = SDL_GetSurfaceProperties(surface);
    Uint64 generation =
        SDL_GetNumberProperty(properties, GPU_RENDERER_SURFACE_GENERATION_PROPERTY, 0);
    gpu_surface_texture_t *cached =
        SDL_GetPointerProperty(properties, GPU_RENDERER_SURFACE_TEXTURE_PROPERTY, NULL);
    if (cached != NULL && generation != 0 && cached->generation == generation) {
        return cached;
    }
    if (cached != NULL) {
        SDL_ClearProperty(properties, GPU_RENDERER_SURFACE_TEXTURE_PROPERTY);
    }

    gpu_surface_texture_t *entry = xcalloc(1, sizeof(*entry));
    bool atlas_attempted;
    entry->atlased = gpu_renderer_texture_atlas_place(surface,
                                                      &entry->texture,
                                                      &entry->atlas_source,
                                                      &entry->atlas,
                                                      &atlas_attempted);
    if (!entry->atlased) {
        if (atlas_attempted) {
            free(entry);
            return NULL;
        }
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
    gpu_renderer_statistics_source_upload(upload_bytes);
    if (!entry->atlased) {
        statistics.resource_creations++;
        statistics.retained_bytes += entry->bytes;
        statistics.peak_retained_bytes =
            MAX(statistics.peak_retained_bytes, statistics.retained_bytes);
        entry->accounted = true;
    }
    entry->next = surface_textures;
    surface_textures = entry;
    if (!SDL_SetNumberProperty(properties,
                               GPU_RENDERER_SURFACE_GENERATION_PROPERTY,
                               (Sint64)entry->generation)) {
        surface_textures = entry->next;
        gpu_renderer_surface_texture_destroy(entry);
        return NULL;
    }
    if (!SDL_SetPointerPropertyWithCleanup(properties,
                                           GPU_RENDERER_SURFACE_TEXTURE_PROPERTY,
                                           entry,
                                           gpu_renderer_surface_texture_cleanup,
                                           NULL)) {
        /* SDL owns the failed value long enough to invoke the cleanup callback. */
        SDL_SetNumberProperty(properties, GPU_RENDERER_SURFACE_GENERATION_PROPERTY, 0);
        return NULL;
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
    if (canvas->texture == NULL) {
        return false;
    }
    canvas->bytes = (size_t)canvas->surface->w * (size_t)canvas->surface->h * 4U;
    statistics.resource_creations++;
    statistics.retained_bytes += canvas->bytes;
    statistics.peak_retained_bytes = MAX(statistics.peak_retained_bytes, statistics.retained_bytes);
    canvas->accounted = true;
    if (!SDL_SetTextureScaleMode(canvas->texture, SDL_SCALEMODE_NEAREST) ||
        !SDL_SetTextureBlendMode(canvas->texture, SDL_BLENDMODE_BLEND)) {
        gpu_renderer_canvas_texture_destroy(canvas);
        return false;
    }

    /* A widget's initial background is immutable upload data. Subsequent
     * widget composition targets this GPU texture directly. */
    SDL_Texture *bootstrap = SDL_CreateTextureFromSurface(renderer, canvas->surface);
    SDL_Texture *previous = SDL_GetRenderTarget(renderer);
    SDL_Rect previous_clip;
    bool previous_clip_enabled = SDL_RenderClipEnabled(renderer);
    if (previous_clip_enabled) {
        SDL_GetRenderClipRect(renderer, &previous_clip);
    }
    bool success =
        bootstrap != NULL && SDL_SetTextureBlendMode(bootstrap, SDL_BLENDMODE_NONE) &&
        SDL_SetRenderTarget(renderer, canvas->texture) && SDL_SetRenderClipRect(renderer, NULL) &&
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, SDL_ALPHA_TRANSPARENT) &&
        SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE) && SDL_RenderClear(renderer) &&
        SDL_RenderTexture(renderer, bootstrap, NULL, NULL) &&
        SDL_SetRenderTarget(renderer, previous) &&
        SDL_SetRenderClipRect(renderer, previous_clip_enabled ? &previous_clip : NULL);
    if (bootstrap != NULL) {
        SDL_DestroyTexture(bootstrap);
    }
    if (!success) {
        SDL_SetRenderTarget(renderer, previous);
        SDL_SetRenderClipRect(renderer, previous_clip_enabled ? &previous_clip : NULL);
        gpu_renderer_canvas_texture_destroy(canvas);
        return false;
    }
    gpu_renderer_statistics_source_upload(canvas->bytes);
    return true;
}

bool gpu_renderer_canvas_register(SDL_Surface **surface_pointer) {
    if (surface_pointer == NULL || *surface_pointer == NULL) {
        gpu_renderer_recreation_request();
        return gpu_renderer_frame_result(false);
    }
    SDL_Surface *surface = *surface_pointer;
    gpu_canvas_t *canvas = gpu_renderer_canvas(surface);
    if (canvas == NULL) {
        canvas = xcalloc(1, sizeof(*canvas));
        canvas->surface = surface;
        canvas->next = canvases;
        canvases = canvas;
        bool injected = false;
#ifdef ATRINIK_GPU_CONFORMANCE_TESTS
        injected =
            gpu_renderer_conformance_fault_take(GPU_RENDERER_CONFORMANCE_FAULT_CANVAS_REGISTRATION);
#endif
        if (injected) {
            gpu_renderer_canvas_cleanup(NULL, canvas);
            canvas = NULL;
        } else if (SDL_SetPointerPropertyWithCleanup(SDL_GetSurfaceProperties(surface),
                                                     GPU_RENDERER_CANVAS_PROPERTY,
                                                     canvas,
                                                     gpu_renderer_canvas_cleanup,
                                                     NULL)) {
            canvas = gpu_renderer_canvas(surface);
        } else {
            /* SDL invokes the supplied cleanup callback when setting fails. */
            canvas = NULL;
        }
        if (canvas == NULL) {
            gpu_renderer_recreation_request();
            gpu_renderer_frame_result(false);
            SDL_DestroySurface(surface);
            *surface_pointer = NULL;
            return false;
        }
    }
    if (renderer != NULL && !gpu_renderer_canvas_texture_create(canvas)) {
        gpu_renderer_recreation_request();
        gpu_renderer_frame_result(false);
        SDL_DestroySurface(surface);
        *surface_pointer = NULL;
        return false;
    }
    return true;
}

bool gpu_renderer_canvas_registered(SDL_Surface *surface) {
    return gpu_renderer_canvas(surface) != NULL;
}

typedef struct gpu_renderer_target_scope {
    SDL_Texture *previous_target;
    SDL_Rect previous_clip;
    bool previous_clip_enabled;
} gpu_renderer_target_scope_t;

static bool gpu_renderer_target_begin(SDL_Surface *surface, gpu_renderer_target_scope_t *scope) {
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
    if (!SDL_SetRenderTarget(renderer, canvas->texture)) {
        return false;
    }
    if (!SDL_SetRenderClipRect(renderer, &clip)) {
        SDL_SetRenderTarget(renderer, scope->previous_target);
        SDL_SetRenderClipRect(renderer,
                              scope->previous_clip_enabled ? &scope->previous_clip : NULL);
        return false;
    }
    return true;
}

static bool gpu_renderer_target_end(const gpu_renderer_target_scope_t *scope, bool success) {
    bool target_restored = SDL_SetRenderTarget(renderer, scope->previous_target);
    bool clip_restored =
        SDL_SetRenderClipRect(renderer,
                              scope->previous_clip_enabled ? &scope->previous_clip : NULL);
    bool restored = target_restored && clip_restored;
    return success && restored;
}

bool gpu_renderer_draw_map_to(SDL_Surface *target,
                              const SDL_FRect *source,
                              const SDL_FRect *destination,
                              SDL_ScaleMode scale_mode) {
    SDL_Texture *map_target = gpu_map_renderer_texture(true);
    if (renderer == NULL || map_target == NULL || target == NULL || destination == NULL ||
        destination->w <= 0.0f || destination->h <= 0.0f ||
        !gpu_renderer_canvas_registered(target)) {
        return gpu_renderer_frame_result(false);
    }
    gpu_renderer_target_scope_t scope;
    if (!gpu_renderer_target_begin(target, &scope)) {
        return gpu_renderer_frame_result(false);
    }
    bool success = SDL_SetTextureScaleMode(map_target, scale_mode) &&
                   SDL_RenderTexture(renderer, map_target, source, destination);
    statistics.draws++;
    return gpu_renderer_frame_result(gpu_renderer_target_end(&scope, success));
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
        !SDL_SetTextureAlphaMod(texture, alpha) || !SDL_SetTextureBlendMode(texture, blend_mode)) {
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
        source_float =
            (SDL_FRect){(float)source->x, (float)source->y, (float)source->w, (float)source->h};
        source_pointer = &source_float;
    }
    statistics.draws++;
    bool success = SDL_RenderTexture(renderer, texture, source_pointer, destination);
    return target == NULL ? success : gpu_renderer_target_end(&scope, success);
}

bool gpu_renderer_draw_surface(SDL_Surface *surface,
                               const SDL_Rect *source,
                               const SDL_FRect *destination) {
    return gpu_renderer_frame_result(gpu_renderer_draw_surface_to_impl(NULL,
                                                                       surface,
                                                                       source,
                                                                       destination,
                                                                       SDL_SCALEMODE_NEAREST));
}

bool gpu_renderer_draw_surface_to(SDL_Surface *target,
                                  SDL_Surface *surface,
                                  const SDL_Rect *source,
                                  const SDL_FRect *destination) {
    return gpu_renderer_frame_result(gpu_renderer_draw_surface_to_impl(target,
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
                                : (SDL_FRect){0.0f, 0.0f, (float)surface->w, (float)surface->h};
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
    bool success =
        gpu_renderer_draw_color(red, green, blue, alpha) &&
        (filled ? SDL_RenderFillRect(renderer, rectangle) : SDL_RenderRect(renderer, rectangle));
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
    return gpu_renderer_frame_result(renderer != NULL &&
                                     SDL_SetRenderClipRect(renderer, rectangle));
}

void gpu_renderer_invalidate_surface(SDL_Surface *surface) {
    if (surface == NULL) {
        return;
    }
    gpu_map_renderer_invalidate_surface(surface);
    SDL_PropertiesID properties = SDL_GetSurfaceProperties(surface);
    SDL_ClearProperty(properties, GPU_RENDERER_CANVAS_PROPERTY);
    SDL_ClearProperty(properties, GPU_RENDERER_SURFACE_TEXTURE_PROPERTY);
    SDL_SetNumberProperty(properties, GPU_RENDERER_SURFACE_GENERATION_PROPERTY, 0);
}

void gpu_renderer_surface_changed(SDL_Surface *surface) {
    if (surface == NULL) {
        return;
    }
    gpu_map_renderer_invalidate_surface(surface);
    SDL_PropertiesID properties = SDL_GetSurfaceProperties(surface);
    gpu_canvas_t *canvas = gpu_renderer_canvas(surface);
    if (canvas != NULL) {
        gpu_renderer_canvas_texture_destroy(canvas);
    }
    SDL_ClearProperty(properties, GPU_RENDERER_SURFACE_TEXTURE_PROPERTY);
    SDL_SetNumberProperty(properties, GPU_RENDERER_SURFACE_GENERATION_PROPERTY, 0);
}

static gpu_pending_readback_t *gpu_renderer_readback_submit(const SDL_Rect *rect) {
    if (renderer == NULL || device == NULL || frame_target == NULL ||
        !SDL_FlushRenderer(renderer)) {
        return NULL;
    }
    float texture_width, texture_height;
    if (!SDL_GetTextureSize(frame_target, &texture_width, &texture_height)) {
        return NULL;
    }
    SDL_Rect area =
        rect != NULL ? *rect : (SDL_Rect){0, 0, (int)texture_width, (int)texture_height};
    if (area.x < 0 || area.y < 0 || area.w <= 0 || area.h <= 0 || area.x > (int)texture_width ||
        area.y > (int)texture_height || area.w > (int)texture_width - area.x ||
        area.h > (int)texture_height - area.y) {
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

    size_t row_bytes;
    size_t row_pitch_size;
    if ((size_t)area.w > (SIZE_MAX - 255U) / 4U) {
        SDL_SetError("GPU screenshot row is too large");
        return NULL;
    }
    row_bytes = (size_t)area.w * 4U;
    row_pitch_size = (row_bytes + 255U) & ~(size_t)255U;
    if (row_pitch_size > UINT32_MAX || (size_t)area.h > UINT32_MAX / row_pitch_size) {
        SDL_SetError("GPU screenshot is too large");
        return NULL;
    }
    Uint32 row_pitch = (Uint32)row_pitch_size;
    Uint32 transfer_size = row_pitch * (Uint32)area.h;
    SDL_GPUTransferBufferCreateInfo create_info = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD,
        .size = transfer_size,
    };
    gpu_pending_readback_t *pending = calloc(1, sizeof(*pending));
    if (pending == NULL) {
        return NULL;
    }
    pending->area = area;
    pending->row_pitch = row_pitch;
    pending->transfer = SDL_CreateGPUTransferBuffer(device, &create_info);
    SDL_GPUCommandBuffer *commands =
        pending->transfer != NULL ? SDL_AcquireGPUCommandBuffer(device) : NULL;
    SDL_GPUCopyPass *copy = commands != NULL ? SDL_BeginGPUCopyPass(commands) : NULL;
    if (copy == NULL) {
        if (commands != NULL) {
            SDL_CancelGPUCommandBuffer(commands);
        }
        if (pending->transfer != NULL) {
            SDL_ReleaseGPUTransferBuffer(device, pending->transfer);
        }
        free(pending);
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
        .transfer_buffer = pending->transfer,
        .pixels_per_row = row_pitch / 4U,
        .rows_per_layer = (Uint32)area.h,
    };
    SDL_DownloadFromGPUTexture(copy, &source, &destination);
    SDL_EndGPUCopyPass(copy);
    pending->fence = SDL_SubmitGPUCommandBufferAndAcquireFence(commands);
    if (pending->fence == NULL) {
        SDL_ReleaseGPUTransferBuffer(device, pending->transfer);
        free(pending);
        return NULL;
    }
    return pending;
}

static SDL_Surface *gpu_renderer_readback_finish(gpu_pending_readback_t *pending) {
    SDL_Surface *surface =
        SDL_CreateSurface(pending->area.w, pending->area.h, SDL_PIXELFORMAT_RGBA32);
    void *pixels = SDL_MapGPUTransferBuffer(device, pending->transfer, false);
    if (surface != NULL && pixels != NULL) {
        for (int y = 0; y < pending->area.h; y++) {
            memcpy((Uint8 *)surface->pixels + (size_t)y * (size_t)surface->pitch,
                   (const Uint8 *)pixels + (size_t)y * pending->row_pitch,
                   (size_t)pending->area.w * 4U);
        }
    } else {
        SDL_DestroySurface(surface);
        surface = NULL;
    }
    if (pixels != NULL) {
        SDL_UnmapGPUTransferBuffer(device, pending->transfer);
    }
    SDL_ReleaseGPUFence(device, pending->fence);
    SDL_ReleaseGPUTransferBuffer(device, pending->transfer);
    free(pending);
    return surface;
}

bool gpu_renderer_readback_async(const SDL_Rect *rect,
                                 gpu_renderer_readback_callback_t callback,
                                 gpu_renderer_readback_cancel_callback_t cancel_callback,
                                 void *userdata) {
    HARD_ASSERT(callback != NULL);
    statistics.readbacks++;
#ifdef ATRINIK_GPU_CONFORMANCE_TESTS
    if (gpu_renderer_conformance_fault_take(GPU_RENDERER_CONFORMANCE_FAULT_READBACK)) {
        return false;
    }
#endif
    gpu_pending_readback_t *pending = gpu_renderer_readback_submit(rect);
    if (pending == NULL) {
        return false;
    }
    pending->callback = callback;
    pending->cancel_callback = cancel_callback;
    pending->userdata = userdata;
    gpu_pending_readback_t **tail = &pending_readbacks;
    while (*tail != NULL) {
        tail = &(*tail)->next;
    }
    *tail = pending;
    return true;
}

void gpu_renderer_readback_poll(void) {
    gpu_pending_readback_t **link = &pending_readbacks;
    while (*link != NULL) {
        gpu_pending_readback_t *pending = *link;
        if (!SDL_QueryGPUFence(device, pending->fence)) {
            link = &pending->next;
            continue;
        }
        *link = pending->next;
        gpu_renderer_readback_callback_t callback = pending->callback;
        void *userdata = pending->userdata;
        SDL_Surface *surface = gpu_renderer_readback_finish(pending);
        callback(surface, userdata);
    }
}

SDL_Surface *gpu_renderer_readback(const SDL_Rect *rect) {
    statistics.readbacks++;
#ifdef ATRINIK_GPU_CONFORMANCE_TESTS
    if (gpu_renderer_conformance_fault_take(GPU_RENDERER_CONFORMANCE_FAULT_READBACK)) {
        return NULL;
    }
#endif
    gpu_pending_readback_t *pending = gpu_renderer_readback_submit(rect);
    if (pending == NULL) {
        return NULL;
    }
    if (!SDL_WaitForGPUFences(device, true, &pending->fence, 1)) {
        SDL_ReleaseGPUFence(device, pending->fence);
        SDL_ReleaseGPUTransferBuffer(device, pending->transfer);
        free(pending);
        return NULL;
    }
    return gpu_renderer_readback_finish(pending);
}

Uint64 gpu_renderer_surface_generation(SDL_Surface *surface) {
    if (surface == NULL) {
        return 0;
    }
    return (Uint64)SDL_GetNumberProperty(SDL_GetSurfaceProperties(surface),
                                         GPU_RENDERER_SURFACE_GENERATION_PROPERTY,
                                         0);
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

static void gpu_renderer_statistics_upload(size_t bytes) {
    statistics.upload_count++;
    statistics.upload_bytes += bytes;
}

void gpu_renderer_statistics_source_upload(size_t bytes) {
    gpu_renderer_statistics_upload(bytes);
    statistics.source_upload_count++;
    statistics.source_upload_bytes += bytes;
}

void gpu_renderer_statistics_instance_upload(size_t bytes) {
    gpu_renderer_statistics_upload(bytes);
    statistics.instance_upload_count++;
    statistics.instance_upload_bytes += bytes;
}

void gpu_renderer_statistics_light_upload(size_t bytes) {
    gpu_renderer_statistics_upload(bytes);
    statistics.light_upload_count++;
    statistics.light_upload_bytes += bytes;
}

void gpu_renderer_statistics_slot_uniform_upload(size_t bytes) {
    gpu_renderer_statistics_upload(bytes);
    statistics.slot_uniform_upload_count++;
    statistics.slot_uniform_upload_bytes += bytes;
}

void gpu_renderer_statistics_resource_create(size_t retained_bytes) {
    statistics.resource_creations++;
    statistics.retained_bytes += retained_bytes;
    statistics.peak_retained_bytes = MAX(statistics.peak_retained_bytes, statistics.retained_bytes);
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
